#include "msx_disk.h"

#include <cstdio>
#include <cstring>

#include "msx_cpu.h"
#include "msx_memory.h"

#ifndef MSX_DISK_LOG_ENABLED
#define MSX_DISK_LOG_ENABLED 0
#endif

#ifndef MSX_DISK_TRACE_LIMIT
#define MSX_DISK_TRACE_LIMIT 16u
#endif

#ifndef MSX_DISK_WD_TRACE_LIMIT
#define MSX_DISK_WD_TRACE_LIMIT 96u
#endif

#if MSX_DISK_LOG_ENABLED
#define MSX_DISK_LOG(...) std::printf(__VA_ARGS__)
#else
#define MSX_DISK_LOG(...) do { } while (0)
#endif

namespace {

struct MsxDiskSlotSnapshot {
    uint8_t slotRegister;
    uint8_t secondarySlotRegs[4];
};

struct MsxDiskMediaInfo {
    uint16_t sectors;
    uint8_t heads;
    uint8_t directoryEntries;
    uint8_t sectorsPerTrack;
    uint8_t sectorsPerFat;
    uint8_t sectorsPerCluster;
};

constexpr MsxDiskMediaInfo kMsxDiskMediaInfo[8] = {
    {  720u, 1u, 112u, 9u, 2u, 2u }, // F8
    { 1440u, 2u, 112u, 9u, 3u, 2u }, // F9
    {  640u, 1u, 112u, 8u, 1u, 2u }, // FA
    { 1280u, 2u, 112u, 8u, 2u, 2u }, // FB
    {  360u, 1u,  64u, 9u, 2u, 1u }, // FC
    {  720u, 2u, 112u, 9u, 2u, 2u }, // FD
    {  320u, 1u,  64u, 8u, 1u, 1u }, // FE
    {  640u, 2u, 112u, 8u, 1u, 2u }, // FF
};

uint32_t s_phydioTraceCount = 0u;
uint32_t s_dskchgTraceCount = 0u;
uint32_t s_getdpbTraceCount = 0u;
uint32_t s_wdPortTraceCount = 0u;
uint32_t s_wdCommandTraceCount = 0u;

uint32_t msx_disk_total_logical_sectors(const MsxDiskState* state)
{
    if (!state) {
        return 0u;
    }

    return static_cast<uint32_t>(state->dskSize / kMsxDskSectorSize);
}

const MsxDiskMediaInfo* msx_disk_media_info(uint8_t mediaDescriptor)
{
    if (mediaDescriptor < 0xF8u) {
        return nullptr;
    }

    const uint8_t index = static_cast<uint8_t>(mediaDescriptor - 0xF8u);
    if (index >= static_cast<uint8_t>(sizeof(kMsxDiskMediaInfo) / sizeof(kMsxDiskMediaInfo[0]))) {
        return nullptr;
    }

    return &kMsxDiskMediaInfo[index];
}

uint8_t msx_disk_media_from_total_sectors(uint32_t totalSectors)
{
    for (uint8_t i = 0u; i < static_cast<uint8_t>(sizeof(kMsxDiskMediaInfo) / sizeof(kMsxDiskMediaInfo[0])); ++i) {
        if (kMsxDiskMediaInfo[i].sectors == totalSectors) {
            return static_cast<uint8_t>(0xF8u + i);
        }
    }

    return 0xF9u;
}

bool msx_disk_trace_slot(uint32_t* counter)
{
    if (!counter) {
        return false;
    }

    if (*counter >= static_cast<uint32_t>(MSX_DISK_TRACE_LIMIT)) {
        return false;
    }

    ++(*counter);
    return true;
}

bool msx_disk_trace_wd(uint32_t* counter)
{
    if (!counter) {
        return false;
    }

    if (*counter >= static_cast<uint32_t>(MSX_DISK_WD_TRACE_LIMIT)) {
        return false;
    }

    ++(*counter);
    return true;
}

const char* msx_disk_wd_command_name(uint8_t cmd)
{
    switch (cmd & 0xF0u) {
        case 0x00u:
            return "RESTORE";
        case 0x10u:
            return "SEEK";
        case 0x20u:
        case 0x30u:
            return "STEP";
        case 0x40u:
        case 0x50u:
            return "STEP-IN";
        case 0x60u:
        case 0x70u:
            return "STEP-OUT";
        case 0x80u:
        case 0x90u:
            return "READ-SECTOR";
        case 0xA0u:
        case 0xB0u:
            return "WRITE-SECTOR";
        case 0xC0u:
            return "READ-ADDRESS";
        case 0xD0u:
            return "FORCE-INT";
        case 0xE0u:
            return "READ-TRACK";
        case 0xF0u:
            return "WRITE-TRACK";
        default:
            return "UNKNOWN";
    }
}

void msx_disk_log_boot_sector_summary(const uint8_t* boot, const char* prefix)
{
    if (!boot) {
        return;
    }

    const uint16_t bytesPerSector =
        static_cast<uint16_t>(boot[0x0Bu] | (static_cast<uint16_t>(boot[0x0Cu]) << 8));
    const uint16_t reservedSectors =
        static_cast<uint16_t>(boot[0x0Eu] | (static_cast<uint16_t>(boot[0x0Fu]) << 8));
    const uint16_t rootEntries =
        static_cast<uint16_t>(boot[0x11u] | (static_cast<uint16_t>(boot[0x12u]) << 8));
    const uint16_t totalSectors =
        static_cast<uint16_t>(boot[0x13u] | (static_cast<uint16_t>(boot[0x14u]) << 8));
    const uint16_t sectorsPerFat =
        static_cast<uint16_t>(boot[0x16u] | (static_cast<uint16_t>(boot[0x17u]) << 8));
    const uint16_t signature =
        static_cast<uint16_t>(boot[0x1FEu] | (static_cast<uint16_t>(boot[0x1FFu]) << 8));

    MSX_DISK_LOG(
        "%s jump=%02X %02X %02X bps=%u spc=%u reserved=%u fats=%u roots=%u total=%u media=%02X spf=%u sig=%04X\n",
        prefix ? prefix : "[MSX][DSK] boot",
        static_cast<unsigned>(boot[0x00u]),
        static_cast<unsigned>(boot[0x01u]),
        static_cast<unsigned>(boot[0x02u]),
        static_cast<unsigned>(bytesPerSector),
        static_cast<unsigned>(boot[0x0Du]),
        static_cast<unsigned>(reservedSectors),
        static_cast<unsigned>(boot[0x10u]),
        static_cast<unsigned>(rootEntries),
        static_cast<unsigned>(totalSectors),
        static_cast<unsigned>(boot[0x15u]),
        static_cast<unsigned>(sectorsPerFat),
        static_cast<unsigned>(signature)
    );
}

uint8_t msx_disk_current_side(const MsxDiskState* state)
{
    return state->driveAndSide & 0x01u;
}

uint8_t msx_disk_irq_drq(const MsxDiskState* state)
{
    if (!state || !state->ready) {
        return 0x00u;
    }

    const uint16_t transferSize =
        state->transferSize ? state->transferSize : static_cast<uint16_t>(kMsxDskSectorSize);
    if (state->bufPos < transferSize) {
        return 0x40u; // DRQ
    }

    return 0x80u; // IRQ, commands complete immediately in this lightweight model.
}

void msx_disk_set_side(MsxDiskState* state, uint8_t side)
{
    if (!state) {
        return;
    }
    state->driveAndSide = static_cast<uint8_t>((state->driveAndSide & 0xFEu) | (side & 0x01u));
}

void msx_disk_set_drive(MsxDiskState* state, uint8_t drive)
{
    if (!state) {
        return;
    }
    state->driveAndSide = static_cast<uint8_t>((state->driveAndSide & 0xF9u) | ((drive & 0x03u) << 1));
}

// Loads the sector identified by (trackReg, sectorReg, side) into sectorBuf.
// Returns true on success.
bool msx_disk_load_sector(MsxDiskState* state)
{
    if (!state->dskData || state->dskSize == 0) {
        MSX_DISK_LOG("[MSX][DSK] wd load fail: no disk data size=%u\n",
                     static_cast<unsigned>(state ? state->dskSize : 0u));
        return false;
    }

    const uint8_t side   = msx_disk_current_side(state);
    const uint8_t track  = state->trackReg;
    const uint8_t sector = state->sectorReg; // 1-based

    if (track >= kMsxDskTracks) {
        MSX_DISK_LOG("[MSX][DSK] wd load fail: track=%u out of range sector=%u side=%u\n",
                     static_cast<unsigned>(track),
                     static_cast<unsigned>(sector),
                     static_cast<unsigned>(side));
        return false;
    }
    if (sector == 0u || sector > kMsxDskSectorsPerTrack) {
        MSX_DISK_LOG("[MSX][DSK] wd load fail: sector=%u invalid track=%u side=%u\n",
                     static_cast<unsigned>(sector),
                     static_cast<unsigned>(track),
                     static_cast<unsigned>(side));
        return false;
    }
    if (side >= state->sides) {
        // Invalid side; treat as record-not-found
        MSX_DISK_LOG("[MSX][DSK] wd load fail: side=%u invalid available=%u track=%u sector=%u driveSel=%02X\n",
                     static_cast<unsigned>(side),
                     static_cast<unsigned>(state->sides),
                     static_cast<unsigned>(track),
                     static_cast<unsigned>(sector),
                     static_cast<unsigned>(state->driveAndSide));
        return false;
    }

    // Flat sector index: (track * sides + side) * sectorsPerTrack + (sector - 1)
    const size_t flatSector =
        (static_cast<size_t>(track) * state->sides + static_cast<size_t>(side))
        * kMsxDskSectorsPerTrack
        + static_cast<size_t>(sector - 1u);
    const size_t byteOffset = flatSector * kMsxDskSectorSize;

    if (byteOffset + kMsxDskSectorSize > state->dskSize) {
        MSX_DISK_LOG("[MSX][DSK] wd load fail: byteOffset=%u beyond size=%u track=%u side=%u sector=%u\n",
                     static_cast<unsigned>(byteOffset),
                     static_cast<unsigned>(state->dskSize),
                     static_cast<unsigned>(track),
                     static_cast<unsigned>(side),
                     static_cast<unsigned>(sector));
        return false;
    }

    std::memcpy(state->sectorBuf, state->dskData + byteOffset, kMsxDskSectorSize);
    return true;
}

void msx_disk_capture_slot_snapshot(const MsxMemoryState* memory, MsxDiskSlotSnapshot* snapshot)
{
    if (!memory || !snapshot) {
        return;
    }

    snapshot->slotRegister = memory->slotRegister;
    std::memcpy(snapshot->secondarySlotRegs,
                memory->secondarySlotRegs,
                sizeof(snapshot->secondarySlotRegs));
}

void msx_disk_restore_slot_snapshot(MsxMemoryState* memory, const MsxDiskSlotSnapshot* snapshot)
{
    if (!memory || !snapshot) {
        return;
    }

    memory->slotRegister = snapshot->slotRegister;
    std::memcpy(memory->secondarySlotRegs,
                snapshot->secondarySlotRegs,
                sizeof(memory->secondarySlotRegs));
    memory->lastPortA8 = memory->slotRegister;
    msx_memory_refresh_maps(memory);
}

void msx_disk_map_all_pages_to_ram(MsxMemoryState* memory)
{
    if (!memory) {
        return;
    }

    memory->slotRegister = 0xFFu;
    std::memset(memory->secondarySlotRegs, 0, sizeof(memory->secondarySlotRegs));
    memory->secondarySlotRegs[3] = 0xAAu;
    memory->lastPortA8 = memory->slotRegister;
    msx_memory_refresh_maps(memory);
}

void msx_disk_execute_command(MsxDiskState* state, uint8_t cmd)
{
    // Upper nibble determines the command type.
    const uint8_t type = cmd & 0xF0u;
    const bool traceCall = msx_disk_trace_wd(&s_wdCommandTraceCount);

    state->lastCommand = cmd;

    if (traceCall) {
        MSX_DISK_LOG("[MSX][WD] CMD %02X %-11s trk=%u sec=%u side=%u drvsel=%02X data=%02X dir=%s\n",
                     static_cast<unsigned>(cmd),
                     msx_disk_wd_command_name(cmd),
                     static_cast<unsigned>(state->trackReg),
                     static_cast<unsigned>(state->sectorReg),
                     static_cast<unsigned>(msx_disk_current_side(state)),
                     static_cast<unsigned>(state->driveAndSide),
                     static_cast<unsigned>(state->dataReg),
                     state->stepDirectionIn ? "in" : "out");
    }

    switch (type) {
        case 0x00u: {
            // Restore: seek to track 0
            state->transferSize = 0u;
            state->bufPos = static_cast<uint16_t>(kMsxDskSectorSize);
            state->trackReg = 0u;
            state->stepDirectionIn = false;
            state->statusReg = kWd2793St1Track0;
            break;
        }
        case 0x10u: {
            // Seek: move to track in dataReg
            state->transferSize = 0u;
            state->bufPos = static_cast<uint16_t>(kMsxDskSectorSize);
            state->stepDirectionIn = state->dataReg >= state->trackReg;
            state->trackReg = state->dataReg;
            state->statusReg = (state->trackReg == 0u) ? kWd2793St1Track0 : 0x00u;
            break;
        }
        case 0x20u:
        case 0x30u: {
            state->transferSize = 0u;
            state->bufPos = static_cast<uint16_t>(kMsxDskSectorSize);
            if (state->stepDirectionIn) {
                if (state->trackReg + 1u < kMsxDskTracks) {
                    ++state->trackReg;
                }
            } else if (state->trackReg > 0u) {
                --state->trackReg;
            }
            state->statusReg = (state->trackReg == 0u) ? kWd2793St1Track0 : 0x00u;
            break;
        }
        case 0x40u:
        case 0x50u: {
            state->transferSize = 0u;
            state->bufPos = static_cast<uint16_t>(kMsxDskSectorSize);
            state->stepDirectionIn = true;
            if (state->trackReg + 1u < kMsxDskTracks) {
                ++state->trackReg;
            }
            state->statusReg = (state->trackReg == 0u) ? kWd2793St1Track0 : 0x00u;
            break;
        }
        case 0x60u:
        case 0x70u: {
            state->transferSize = 0u;
            state->bufPos = static_cast<uint16_t>(kMsxDskSectorSize);
            state->stepDirectionIn = false;
            if (state->trackReg > 0u) {
                --state->trackReg;
            }
            state->statusReg = (state->trackReg == 0u) ? kWd2793St1Track0 : 0x00u;
            break;
        }
        case 0x80u: // Read Sector (E=0)
        case 0x90u: { // Read Sector (E=1, head settle delay – ignored in emulation)
            state->bufPos = static_cast<uint16_t>(kMsxDskSectorSize); // invalidate first
            if (msx_disk_load_sector(state)) {
                state->transferSize = static_cast<uint16_t>(kMsxDskSectorSize);
                state->bufPos  = 0u;
                state->statusReg = kWd2793St2Busy | kWd2793St2Drq;
            } else {
                state->transferSize = 0u;
                state->statusReg = kWd2793St2RecordNotFound;
            }
            break;
        }
        case 0xA0u: // Write Sector (E=0)
        case 0xB0u: { // Write Sector (E=1)
            // Write-protected: DSK image is read-only (XIP-mapped flash).
            state->transferSize = static_cast<uint16_t>(kMsxDskSectorSize);
            state->bufPos    = static_cast<uint16_t>(kMsxDskSectorSize);
            state->statusReg = kWd2793St2WriteProtect;
            break;
        }
        case 0xC0u: { // Read Address
            state->sectorBuf[0] = state->trackReg;
            state->sectorBuf[1] = msx_disk_current_side(state);
            state->sectorBuf[2] = state->sectorReg ? state->sectorReg : 1u;
            state->sectorBuf[3] = 0x02u;
            state->sectorBuf[4] = 0x00u;
            state->sectorBuf[5] = 0x00u;
            state->transferSize = 6u;
            state->bufPos = 0u;
            state->statusReg = kWd2793St2Busy | kWd2793St2Drq;
            break;
        }
        case 0xE0u: { // Read Track
            state->transferSize = 0u;
            state->bufPos = static_cast<uint16_t>(kMsxDskSectorSize);
            state->statusReg = kWd2793St2RecordNotFound;
            break;
        }
        case 0xF0u: { // Write Track
            state->transferSize = 0u;
            state->bufPos = static_cast<uint16_t>(kMsxDskSectorSize);
            state->statusReg = kWd2793St2WriteProtect;
            break;
        }
        case 0xD0u: // Force Interrupt
        default: {
            // Clear any pending operation.
            state->transferSize = 0u;
            state->bufPos    = static_cast<uint16_t>(kMsxDskSectorSize);
            state->statusReg = (state->trackReg == 0u) ? kWd2793St1Track0 : 0x00u;
            break;
        }
    }

    if (traceCall) {
        MSX_DISK_LOG("[MSX][WD] CMD result status=%02X trk=%u sec=%u bufPos=%u transfer=%u\n",
                     static_cast<unsigned>(state->statusReg),
                     static_cast<unsigned>(state->trackReg),
                     static_cast<unsigned>(state->sectorReg),
                     static_cast<unsigned>(state->bufPos),
                     static_cast<unsigned>(state->transferSize));
    }
}

} // namespace

void msx_disk_init(MsxDiskState* state, const uint8_t* dskData, size_t dskSize)
{
    if (!state) {
        return;
    }

    std::memset(state, 0, sizeof(*state));
    state->dskData = dskData;
    state->dskSize = dskSize;

    if (dskSize > 0u) {
        // 1DD = 80 * 1 * 9 * 512 = 368640 bytes; anything larger is 2DD.
        constexpr size_t kSingleSidedSize =
            static_cast<size_t>(kMsxDskTracks) *
            static_cast<size_t>(kMsxDskSectorsPerTrack) *
            kMsxDskSectorSize;
        state->sides = (dskSize > kSingleSidedSize) ? 2u : 1u;

        constexpr size_t kDoubleSidedSize = kSingleSidedSize * 2u;
        const size_t totalSectors = dskSize / kMsxDskSectorSize;
        const size_t remainder = dskSize % kMsxDskSectorSize;
        const bool exactKnown = (dskSize == kSingleSidedSize) || (dskSize == kDoubleSidedSize);
        MSX_DISK_LOG("[MSX][DSK] init size=%u sectors=%u remainder=%u sides=%u exact=%s\n",
                     static_cast<unsigned>(dskSize),
                     static_cast<unsigned>(totalSectors),
                     static_cast<unsigned>(remainder),
                     static_cast<unsigned>(state->sides),
                     exactKnown ? "yes" : "no");

        if (dskData && dskSize >= kMsxDskSectorSize) {
            uint8_t boot[kMsxDskSectorSize] = {};
            std::memcpy(boot, dskData, kMsxDskSectorSize);
            msx_disk_log_boot_sector_summary(boot, "[MSX][DSK] init boot");
        }
    } else {
        state->sides = 1u;
        MSX_DISK_LOG("[MSX][DSK] init with empty image\n");
    }

    state->sectorReg = 1u;
    state->transferSize = 0u;
    state->bufPos    = static_cast<uint16_t>(kMsxDskSectorSize);
    state->statusReg = kWd2793St1Track0; // starts at track 0
    state->dskchgKnown = false;
    state->lastCommand = 0u;
    state->stepDirectionIn = false;
    state->ready     = true;
}

void msx_disk_reset(MsxDiskState* state)
{
    if (!state || !state->ready) {
        return;
    }

    state->trackReg    = 0u;
    state->sectorReg   = 1u;
    state->dataReg     = 0u;
    state->driveAndSide = 0u;
    state->lastCommand = 0u;
    state->stepDirectionIn = false;
    state->transferSize = 0u;
    state->bufPos      = static_cast<uint16_t>(kMsxDskSectorSize);
    state->statusReg   = kWd2793St1Track0;
    state->dskchgKnown = false;
}

uint8_t msx_disk_in(MsxDiskState* state, uint8_t port)
{
    if (!state || !state->ready) {
        return 0xFFu;
    }

    switch (port) {
        case 0xD0u:
            return state->statusReg;

        case 0xD1u:
            return state->trackReg;

        case 0xD2u:
            return state->sectorReg;

        case 0xD3u: {
            // Data register read: stream bytes from sector buffer.
            const uint16_t transferSize =
                state->transferSize ? state->transferSize : static_cast<uint16_t>(kMsxDskSectorSize);
            if (state->bufPos < transferSize) {
                if (state->bufPos == 0u && msx_disk_trace_wd(&s_wdPortTraceCount)) {
                    MSX_DISK_LOG("[MSX][WD] IN D3 stream start cmd=%02X len=%u trk=%u sec=%u side=%u\n",
                                 static_cast<unsigned>(state->lastCommand),
                                 static_cast<unsigned>(transferSize),
                                 static_cast<unsigned>(state->trackReg),
                                 static_cast<unsigned>(state->sectorReg),
                                 static_cast<unsigned>(msx_disk_current_side(state)));
                }
                const uint8_t byte = state->sectorBuf[state->bufPos++];
                if (state->bufPos >= transferSize) {
                    // Sector read complete: clear BUSY and DRQ.
                    if (msx_disk_trace_wd(&s_wdPortTraceCount)) {
                        MSX_DISK_LOG("[MSX][WD] IN D3 stream end cmd=%02X trk=%u sec=%u side=%u\n",
                                     static_cast<unsigned>(state->lastCommand),
                                     static_cast<unsigned>(state->trackReg),
                                     static_cast<unsigned>(state->sectorReg),
                                     static_cast<unsigned>(msx_disk_current_side(state)));
                    }
                    state->transferSize = 0u;
                    state->statusReg = 0x00u;
                }
                return byte;
            }
            return state->dataReg;
        }

        case 0xD4u:
            return msx_disk_irq_drq(state);

        default:
            return 0xFFu;
    }
}

void msx_disk_out(MsxDiskState* state, uint8_t port, uint8_t value)
{
    if (!state || !state->ready) {
        return;
    }

    switch (port) {
        case 0xD0u:
            msx_disk_execute_command(state, value);
            break;
        case 0xD1u:
            if (msx_disk_trace_wd(&s_wdPortTraceCount)) {
                MSX_DISK_LOG("[MSX][WD] OUT D1 track=%02X (prev=%02X)\n",
                             static_cast<unsigned>(value),
                             static_cast<unsigned>(state->trackReg));
            }
            state->trackReg    = value;
            break;
        case 0xD2u:
            if (msx_disk_trace_wd(&s_wdPortTraceCount)) {
                MSX_DISK_LOG("[MSX][WD] OUT D2 sector=%02X (prev=%02X)\n",
                             static_cast<unsigned>(value),
                             static_cast<unsigned>(state->sectorReg));
            }
            state->sectorReg   = value;
            break;
        case 0xD3u:
            if (msx_disk_trace_wd(&s_wdPortTraceCount)) {
                MSX_DISK_LOG("[MSX][WD] OUT D3 data=%02X (prev=%02X)\n",
                             static_cast<unsigned>(value),
                             static_cast<unsigned>(state->dataReg));
            }
            state->dataReg     = value;
            break;
        case 0xD4u:
            if (msx_disk_trace_wd(&s_wdPortTraceCount)) {
                MSX_DISK_LOG("[MSX][WD] OUT D4 drive/side=%02X side=%u drive=%u\n",
                             static_cast<unsigned>(value),
                             static_cast<unsigned>((value & 0x10u) ? 1u : 0u),
                             static_cast<unsigned>((value >> 1) & 0x01u));
            }
            // Brazilian DiskROM I/O layout used by fMSX: [xxxSxxDx],
            // with side inverted in the WD1793 system register.
            msx_disk_set_side(state, (value & 0x10u) ? 1u : 0u);
            msx_disk_set_drive(state, static_cast<uint8_t>((value >> 1) & 0x01u));
            break;
        default:
            break;
    }
}

bool msx_disk_memory_read(MsxDiskState* state, uint16_t address, uint8_t* value)
{
    if (!state || !state->ready || !value) {
        return false;
    }

    switch (address) {
        case 0x7FF8u: case 0xBFF8u: case 0x7F80u: case 0x7FB8u:
        case 0x7FF9u: case 0xBFF9u: case 0x7F81u: case 0x7FB9u:
        case 0x7FFAu: case 0xBFFAu: case 0x7F82u: case 0x7FBAu:
        case 0x7FFBu: case 0xBFFBu: case 0x7F83u: case 0x7FBBu:
            *value = msx_disk_in(state, static_cast<uint8_t>(0xD0u + (address & 0x0003u)));
            return true;

        case 0x7FFFu: case 0xBFFFu: case 0x7F84u: case 0x7FBCu:
            *value = msx_disk_irq_drq(state);
            return true;

        default:
            return false;
    }
}

bool msx_disk_memory_write(MsxDiskState* state, uint16_t address, uint8_t value)
{
    if (!state || !state->ready) {
        return false;
    }

    switch (address) {
        case 0x7FF8u: case 0xBFF8u: case 0x7F80u: case 0x7FB8u:
        case 0x7FF9u: case 0xBFF9u: case 0x7F81u: case 0x7FB9u:
        case 0x7FFAu: case 0xBFFAu: case 0x7F82u: case 0x7FBAu:
        case 0x7FFBu: case 0xBFFBu: case 0x7F83u: case 0x7FBBu:
            msx_disk_out(state, static_cast<uint8_t>(0xD0u + (address & 0x0003u)), value);
            return true;

        case 0x7FFCu: case 0xBFFCu:
            msx_disk_set_side(state, value & 0x01u);
            return true;

        case 0x7FFDu: case 0xBFFDu:
            msx_disk_set_drive(state, static_cast<uint8_t>(value & 0x01u));
            return true;

        case 0x7F84u: case 0x7FBCu:
            msx_disk_set_drive(state, static_cast<uint8_t>(value & 0x03u));
            msx_disk_set_side(state, (value & 0x04u) ? 1u : 0u);
            return true;

        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// BIOS software-trap patch support
// ---------------------------------------------------------------------------

// The DISK ROM entry points occupied by the patches (file offsets = Z80 addr - 0x4000):
//   0x0010 = PHYDIO  (physical disk I/O)
//   0x0013 = DSKCHG  (disk-change check)
//   0x0016 = GETDPB  (get disk parameter block)
//   0x001C = DSKFMT  (format disk)
//   0x001F = DRVOFF  (drive off)

static const uint16_t kDiskRomPatchOffsets[] = { 0x0010u, 0x0013u, 0x0016u, 0x001Cu, 0x001Fu };
static const uint16_t kDiskRomPatchZ80Addrs[] = { 0x4010u, 0x4013u, 0x4016u, 0x401Cu, 0x401Fu };

void msx_disk_apply_rom_patches(uint8_t* diskRomBuf, size_t diskRomSize)
{
    if (!diskRomBuf) {
        return;
    }

    for (uint16_t off : kDiskRomPatchOffsets) {
        if (static_cast<size_t>(off + 3u) > diskRomSize) {
            continue;
        }
        diskRomBuf[off + 0u] = 0xEDu; // custom opcode prefix
        diskRomBuf[off + 1u] = 0xFEu; // custom opcode
        diskRomBuf[off + 2u] = 0xC9u; // RET
    }

    std::printf("[MSX] disk ROM patches applied\n");
}

// Helpers to access Z80 registers from a MsxCpuState pointer.
static inline uint8_t cpu_a(const MsxCpuState* c) { return static_cast<uint8_t>(c->af >> 8); }
static inline uint8_t cpu_f(const MsxCpuState* c) { return static_cast<uint8_t>(c->af & 0xFFu); }
static inline uint8_t cpu_b(const MsxCpuState* c) { return static_cast<uint8_t>(c->bc >> 8); }
static inline void set_a(MsxCpuState* c, uint8_t v) { c->af = static_cast<uint16_t>((c->af & 0x00FFu) | (static_cast<uint16_t>(v) << 8)); }
static inline void set_b(MsxCpuState* c, uint8_t v) { c->bc = static_cast<uint16_t>((c->bc & 0x00FFu) | (static_cast<uint16_t>(v) << 8)); }
static inline void clear_carry(MsxCpuState* c) { c->af &= ~0x01u; }
static inline void set_carry(MsxCpuState* c)   { c->af |= 0x01u; }

// Read one logical sector (0-based) from the DSK image into dest[512].
// Returns true on success.
bool msx_disk_read_logical_sector(const MsxDiskState* disk, uint32_t logicalSector, uint8_t* dest)
{
    if (!disk || !disk->dskData || disk->dskSize == 0u) {
        MSX_DISK_LOG("[MSX][DSK] logical read fail: no disk sector=%lu size=%u\n",
                     static_cast<unsigned long>(logicalSector),
                     static_cast<unsigned>(disk ? disk->dskSize : 0u));
        return false;
    }

    const size_t byteOffset = static_cast<size_t>(logicalSector) * kMsxDskSectorSize;
    if (byteOffset + kMsxDskSectorSize > disk->dskSize) {
        MSX_DISK_LOG("[MSX][DSK] logical read fail: sector=%lu byteOffset=%u beyond size=%u totalSectors=%lu\n",
                     static_cast<unsigned long>(logicalSector),
                     static_cast<unsigned>(byteOffset),
                     static_cast<unsigned>(disk->dskSize),
                     static_cast<unsigned long>(msx_disk_total_logical_sectors(disk)));
        return false;
    }

    std::memcpy(dest, disk->dskData + byteOffset, kMsxDskSectorSize);
    return true;
}

// PHYDIO: Physical disk I/O.
// Entry: A=drive, B=sectors, DE=first logical sector (0-based), HL=buffer addr, CF=0 read/1 write.
// Exit:  CF=0 success (B=0 sectors remaining), CF=1 error (A=error code).
static void patch_phydio(MsxCpuState* cpu, MsxMemoryState* memory)
{
    MsxDiskState* disk = memory->disk;
    const bool isWrite = (cpu_f(cpu) & 0x01u) != 0u;
    uint8_t count = cpu_b(cpu);
    const uint8_t requestedCount = count;
    const uint32_t firstSector = cpu->de;
    uint16_t dstAddr = cpu->hl;
    const bool traceCall = msx_disk_trace_slot(&s_phydioTraceCount);
    const uint32_t totalSectors = msx_disk_total_logical_sectors(disk);
    const uint8_t mediaDescriptor = static_cast<uint8_t>(cpu->bc & 0x00FFu);
    const MsxDiskMediaInfo* mediaInfo = msx_disk_media_info(mediaDescriptor);
    const uint32_t mediaSectors = mediaInfo ? mediaInfo->sectors : totalSectors;

    cpu->iff1 = true;
    cpu->iff2 = true;

    if (traceCall) {
        MSX_DISK_LOG("[MSX][DSK] PHYDIO %s drive=%u count=%u first=%lu dst=%04X media=%02X mediaSectors=%lu totalSectors=%lu\n",
                     isWrite ? "write" : "read",
                     static_cast<unsigned>(cpu_a(cpu)),
                     static_cast<unsigned>(count),
                     static_cast<unsigned long>(firstSector),
                     static_cast<unsigned>(dstAddr),
                     static_cast<unsigned>(mediaDescriptor),
                     static_cast<unsigned long>(mediaSectors),
                     static_cast<unsigned long>(totalSectors));
    }

    if (!disk || !disk->dskData) {
        MSX_DISK_LOG("[MSX][DSK] PHYDIO fail: disk not ready\n");
        cpu->af = 0x0201u; // not ready
        return;
    }

    if (firstSector + count > mediaSectors) {
        MSX_DISK_LOG("[MSX][DSK] PHYDIO fail: first=%lu count=%u exceeds mediaLimit=%lu media=%02X actualSectors=%lu\n",
                     static_cast<unsigned long>(firstSector),
                     static_cast<unsigned>(count),
                     static_cast<unsigned long>(mediaSectors),
                     static_cast<unsigned>(mediaDescriptor),
                     static_cast<unsigned long>(totalSectors));
        cpu->af = 0x0801u; // record not found
        return;
    }

    if (static_cast<uint32_t>(dstAddr) + static_cast<uint32_t>(count) * kMsxDskSectorSize > 0x10000u) {
        count = static_cast<uint8_t>((0x10000u - static_cast<uint32_t>(dstAddr)) / kMsxDskSectorSize);
        MSX_DISK_LOG("[MSX][DSK] PHYDIO clamp: dst=%04X requested=%u effective=%u\n",
                     static_cast<unsigned>(dstAddr),
                     static_cast<unsigned>(requestedCount),
                     static_cast<unsigned>(count));
    }

    MsxDiskSlotSnapshot snapshot = {};
    msx_disk_capture_slot_snapshot(memory, &snapshot);
    msx_disk_map_all_pages_to_ram(memory);

    for (uint8_t i = 0u; i < count; ++i) {
        const uint32_t logicalSector = firstSector + i;

        if (isWrite) {
            msx_disk_restore_slot_snapshot(memory, &snapshot);
            MSX_DISK_LOG("[MSX][DSK] PHYDIO write blocked: sector=%lu dst=%04X\n",
                         static_cast<unsigned long>(logicalSector),
                         static_cast<unsigned>(dstAddr));
            cpu->af = 0x0001u; // write protected
            return;
        }

        const size_t byteOffset = static_cast<size_t>(logicalSector) * kMsxDskSectorSize;
        if (byteOffset + kMsxDskSectorSize > disk->dskSize) {
            msx_disk_restore_slot_snapshot(memory, &snapshot);
            MSX_DISK_LOG("[MSX][DSK] PHYDIO read fail: sector=%lu countIndex=%u\n",
                         static_cast<unsigned long>(logicalSector),
                         static_cast<unsigned>(i));
            cpu->af = 0x0401u; // data error / read failure
            return;
        }

        const uint8_t* src = disk->dskData + byteOffset;
        uint32_t bytesLeft = kMsxDskSectorSize;
        while (bytesLeft > 0) {
            const uint8_t bank = static_cast<uint8_t>(dstAddr >> 13);
            const uint16_t offset = static_cast<uint16_t>(dstAddr & 0x1FFFu);
            const uint32_t maxChunk = static_cast<uint32_t>(0x2000u - offset);
            const uint32_t chunk = bytesLeft < maxChunk ? bytesLeft : maxChunk;

            if (memory->writeMap[bank]) {
                std::memcpy(memory->writeMap[bank] + offset, src, chunk);
            } else {
                for (uint32_t b = 0; b < chunk; ++b) {
                    msx_memory_write8(memory, static_cast<uint16_t>(dstAddr + b), src[b]);
                }
            }
            
            src += chunk;
            dstAddr = static_cast<uint16_t>(dstAddr + chunk);
            bytesLeft -= chunk;
        }
    }

    msx_disk_restore_slot_snapshot(memory, &snapshot);
    set_b(cpu, 0u);
    clear_carry(cpu);

    if (traceCall) {
        MSX_DISK_LOG("[MSX][DSK] PHYDIO ok first=%lu count=%u dstEnd=%04X\n",
                     static_cast<unsigned long>(firstSector),
                     static_cast<unsigned>(count),
                     static_cast<unsigned>(dstAddr));
    }
}

// DSKCHG: Disk-change check.
// For immutable .dsk images we prime the DPB once, then report unchanged.
static bool patch_dskchg(MsxCpuState* cpu, MsxMemoryState* memory)
{
    MsxDiskState* disk = memory ? memory->disk : nullptr;
    const bool traceCall = msx_disk_trace_slot(&s_dskchgTraceCount);

    cpu->iff1 = true;
    cpu->iff2 = true;

    if (traceCall) {
        MSX_DISK_LOG("[MSX][DSK] DSKCHG drive=%u disk=%s known=%u\n",
                     static_cast<unsigned>(cpu_a(cpu)),
                     (disk && disk->dskData) ? "present" : "missing",
                     static_cast<unsigned>(disk && disk->dskchgKnown ? 1u : 0u));
    }

    if (!disk || !disk->dskData) {
        MSX_DISK_LOG("[MSX][DSK] DSKCHG fail: no disk\n");
        cpu->af = 0x0201u; // not ready
        return false;
    }

    if (!disk->dskchgKnown) {
        disk->dskchgKnown = true;
        set_b(cpu, 0u); // unknown
        clear_carry(cpu);
        if (traceCall) {
            MSX_DISK_LOG("[MSX][DSK] DSKCHG result=unknown -> refresh DPB\n");
        }
        return true;
    }

    set_b(cpu, 1u); // unchanged
    clear_carry(cpu);
    if (traceCall) {
        MSX_DISK_LOG("[MSX][DSK] DSKCHG result=unchanged\n");
    }
    return false;
}

// GETDPB: Build Disk Parameter Block from the disk's boot sector BPB.
// Entry: A=drive, B/C=media descriptor, HL=base address of DPB.
// Exit:  HL+1..HL+18 filled, CF=0.
static void patch_getdpb(MsxCpuState* cpu, MsxMemoryState* memory)
{
    MsxDiskState* disk = memory->disk;
    uint8_t boot[kMsxDskSectorSize] = {};
    const bool traceCall = msx_disk_trace_slot(&s_getdpbTraceCount);

    if (!disk || !disk->dskData) {
        MSX_DISK_LOG("[MSX][DSK] GETDPB fail: no disk\n");
        cpu->af = 0x0201u; // not ready
        return;
    }

    if (!msx_disk_read_logical_sector(disk, 0u, boot)) {
        MSX_DISK_LOG("[MSX][DSK] GETDPB fail: boot sector read error\n");
        cpu->af = 0x0C01u; // other error
        return;
    }

    if (traceCall) {
        MSX_DISK_LOG("[MSX][DSK] GETDPB drive=%u mediaBC=%04X hl=%04X\n",
                     static_cast<unsigned>(cpu_a(cpu)),
                     static_cast<unsigned>(cpu->bc),
                     static_cast<unsigned>(cpu->hl));
        msx_disk_log_boot_sector_summary(boot, "[MSX][DSK] GETDPB boot");
    }

    uint8_t mediaDescriptor = boot[0x15u];
    const uint32_t totalLogicalSectors = msx_disk_total_logical_sectors(disk);
    if (!msx_disk_media_info(mediaDescriptor)) {
        mediaDescriptor = msx_disk_media_from_total_sectors(totalLogicalSectors);
    }
    const MsxDiskMediaInfo* mediaInfo = msx_disk_media_info(mediaDescriptor);

    int bytesPerSector  = static_cast<int>(boot[0x0Cu]) * 256 + boot[0x0Bu];
    int sectorsPerCluster = boot[0x0Du];
    int reservedSectors = static_cast<int>(boot[0x0Fu]) * 256 + boot[0x0Eu];
    int fatCount = boot[0x10u];
    int directoryEntries = boot[0x11u];
    int sectorsPerDisk  = static_cast<int>(boot[0x14u]) * 256 + boot[0x13u];
    int sectorsPerFat   = static_cast<int>(boot[0x17u]) * 256 + boot[0x16u];
    const bool validBpb =
        (bytesPerSector == static_cast<int>(kMsxDskSectorSize)) &&
        sectorsPerCluster > 0 &&
        reservedSectors > 0 &&
        fatCount > 0 &&
        directoryEntries > 0 &&
        sectorsPerDisk > 0 &&
        sectorsPerFat > 0;

    if (!validBpb && mediaInfo) {
        MSX_DISK_LOG("[MSX][DSK] GETDPB fallback media=%02X totalSectors=%lu bps=%d spc=%d\n",
                     static_cast<unsigned>(mediaDescriptor),
                     static_cast<unsigned long>(totalLogicalSectors),
                     bytesPerSector,
                     sectorsPerCluster);
        bytesPerSector = static_cast<int>(kMsxDskSectorSize);
        sectorsPerCluster = mediaInfo->sectorsPerCluster;
        reservedSectors = 1;
        fatCount = 2;
        directoryEntries = mediaInfo->directoryEntries;
        sectorsPerDisk = mediaInfo->sectors;
        sectorsPerFat = mediaInfo->sectorsPerFat;
    }

    uint16_t addr = static_cast<uint16_t>(cpu->hl + 1u);
    msx_memory_write8(memory, addr++, mediaDescriptor);
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(bytesPerSector & 0xFF));
    msx_memory_write8(memory, addr++, static_cast<uint8_t>((bytesPerSector >> 8) & 0xFF));

    int value = (bytesPerSector >> 5) - 1;
    int shift = 0;
    while (value & (1 << shift)) {
        ++shift;
    }
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(value));
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(shift));

    value = sectorsPerCluster - 1;
    shift = 0;
    while (value & (1 << shift)) {
        ++shift;
    }
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(value));
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(shift + 1));

    msx_memory_write8(memory, addr++, static_cast<uint8_t>(reservedSectors & 0xFF));
    msx_memory_write8(memory, addr++, static_cast<uint8_t>((reservedSectors >> 8) & 0xFF));
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(fatCount & 0xFF));
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(directoryEntries & 0xFF));

    value = reservedSectors + fatCount * sectorsPerFat;
    value += 32 * directoryEntries / bytesPerSector;
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(value & 0xFF));
    msx_memory_write8(memory, addr++, static_cast<uint8_t>((value >> 8) & 0xFF));

    value = (sectorsPerDisk - value) / sectorsPerCluster;
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(value & 0xFF));
    msx_memory_write8(memory, addr++, static_cast<uint8_t>((value >> 8) & 0xFF));

    msx_memory_write8(memory, addr++, static_cast<uint8_t>(sectorsPerFat & 0xFF));

    value = reservedSectors + fatCount * sectorsPerFat;
    msx_memory_write8(memory, addr++, static_cast<uint8_t>(value & 0xFF));
    msx_memory_write8(memory, addr, static_cast<uint8_t>((value >> 8) & 0xFF));

    clear_carry(cpu);
}

// DSKFMT: Format disk — write-protected, return error.
static void patch_dskfmt(MsxCpuState* cpu, MsxMemoryState* /*memory*/)
{
    cpu->iff1 = true;
    cpu->iff2 = true;
    cpu->af = 0x0001u; // write protected
}

// DRVOFF: Drive off — no-op.
static void patch_drvoff(MsxCpuState* cpu, MsxMemoryState* /*memory*/)
{
    clear_carry(cpu);
}

// ---------------------------------------------------------------------------
// CAS cassette tape implementation
// ---------------------------------------------------------------------------

const uint8_t kMsxCasHeader[8] = { 0x1F, 0xA6, 0xDE, 0xBA, 0xCC, 0x13, 0x7D, 0x74 };

void msx_cas_init(MsxCasState* state, const uint8_t* data, size_t size)
{
    if (!state) {
        return;
    }

    state->casData = data;
    state->casSize = size;
    state->casPos  = 0;
    state->ready   = (data != nullptr && size >= 8u);
}

// TAPION: search forward (aligned to 8 bytes) for the CAS block header.
// CF=0 on success (position is just past the header), CF=1 on failure (rewound).
static void patch_tapion(MsxCpuState* cpu, MsxCasState* cas)
{
    if (!cas || !cas->ready || !cas->casData) {
        set_carry(cpu);
        return;
    }

    // Align current position up to the next 8-byte boundary.
    if (cas->casPos & 7u) {
        cas->casPos = (cas->casPos + 8u) & ~static_cast<size_t>(7u);
    }

    while (cas->casPos + 8u <= cas->casSize) {
        if (std::memcmp(cas->casData + cas->casPos, kMsxCasHeader, 8u) == 0) {
            cas->casPos += 8u; // skip past the header
            clear_carry(cpu);  // success
            return;
        }
        cas->casPos += 8u;
    }

    // Header not found — rewind and signal error.
    cas->casPos = 0;
    set_carry(cpu);
}

// TAPIN: read one byte from the tape into A.
// CF=0 on success, CF=1 on EOF (tape rewound).
static void patch_tapin(MsxCpuState* cpu, MsxCasState* cas)
{
    if (!cas || !cas->ready || !cas->casData) {
        set_carry(cpu);
        return;
    }

    if (cas->casPos >= cas->casSize) {
        cas->casPos = 0; // auto-rewind
        set_carry(cpu);
        return;
    }

    set_a(cpu, cas->casData[cas->casPos++]);
    clear_carry(cpu);
}

void msx_disk_bios_patch_handler(MsxCpuState* cpu, MsxMemoryState* memory, uint16_t patchAddress)
{
    if (!cpu || !memory) {
        return;
    }

    MsxCasState* const cas = memory->cas;

    switch (patchAddress) {
        case 0x00E1u: // TAPION – search for next block header
            if (cas && cas->ready) {
                patch_tapion(cpu, cas);
            } else {
                set_carry(cpu);
            }
            break;
        case 0x00E4u: // TAPIN – read one byte
            if (cas && cas->ready) {
                patch_tapin(cpu, cas);
            } else {
                set_carry(cpu);
            }
            break;
        case 0x00EAu: // TAPOON – open tape for write (not supported)
        case 0x00EDu: // TAPOUT – write one byte (not supported)
            set_carry(cpu);
            break;
        case 0x00E7u: // TAPIOF – close tape input (no-op)
        case 0x00F0u: // TAPOOF – close tape output (no-op)
        case 0x00F3u: // STMOTR – motor control (no-op)
            clear_carry(cpu);
            break;
        case 0x4010u: patch_phydio(cpu, memory);          break;
        case 0x4013u:
            if (patch_dskchg(cpu, memory)) {
                patch_getdpb(cpu, memory);
            }
            break;
        case 0x4016u: patch_getdpb(cpu, memory);          break;
        case 0x401Cu: patch_dskfmt(cpu, memory);          break;
        case 0x401Fu: patch_drvoff(cpu, memory);          break;
        default:
            std::printf("[MSX] ED FE at unknown address %04X\n", patchAddress);
            clear_carry(cpu);
            break;
    }
}
