#include "msx_disk.h"

#include <cstdio>
#include <cstring>

#include "msx_cpu.h"
#include "msx_memory.h"

namespace {

uint8_t msx_disk_current_side(const MsxDiskState* state)
{
    return state->driveAndSide & 0x01u;
}

// Loads the sector identified by (trackReg, sectorReg, side) into sectorBuf.
// Returns true on success.
bool msx_disk_load_sector(MsxDiskState* state)
{
    if (!state->dskData || state->dskSize == 0) {
        return false;
    }

    const uint8_t side   = msx_disk_current_side(state);
    const uint8_t track  = state->trackReg;
    const uint8_t sector = state->sectorReg; // 1-based

    if (track >= kMsxDskTracks) {
        return false;
    }
    if (sector == 0u || sector > kMsxDskSectorsPerTrack) {
        return false;
    }
    if (side >= state->sides) {
        // Invalid side; treat as record-not-found
        return false;
    }

    // Flat sector index: (track * sides + side) * sectorsPerTrack + (sector - 1)
    const size_t flatSector =
        (static_cast<size_t>(track) * state->sides + static_cast<size_t>(side))
        * kMsxDskSectorsPerTrack
        + static_cast<size_t>(sector - 1u);
    const size_t byteOffset = flatSector * kMsxDskSectorSize;

    if (byteOffset + kMsxDskSectorSize > state->dskSize) {
        return false;
    }

    std::memcpy(state->sectorBuf, state->dskData + byteOffset, kMsxDskSectorSize);
    return true;
}

void msx_disk_execute_command(MsxDiskState* state, uint8_t cmd)
{
    // Upper nibble determines the command type.
    const uint8_t type = cmd & 0xF0u;

    switch (type) {
        case 0x00u: {
            // Restore: seek to track 0
            state->trackReg = 0u;
            state->statusReg = kWd2793St1Track0;
            break;
        }
        case 0x10u: {
            // Seek: move to track in dataReg
            state->trackReg = state->dataReg;
            state->statusReg = (state->trackReg == 0u) ? kWd2793St1Track0 : 0x00u;
            break;
        }
        case 0x80u: // Read Sector (E=0)
        case 0x90u: { // Read Sector (E=1, head settle delay – ignored in emulation)
            state->bufPos = static_cast<uint16_t>(kMsxDskSectorSize); // invalidate first
            if (msx_disk_load_sector(state)) {
                state->bufPos  = 0u;
                state->statusReg = kWd2793St2Busy | kWd2793St2Drq;
            } else {
                state->statusReg = kWd2793St2RecordNotFound;
            }
            break;
        }
        case 0xA0u: // Write Sector (E=0)
        case 0xB0u: { // Write Sector (E=1)
            // Write-protected: DSK image is read-only (XIP-mapped flash).
            state->bufPos    = static_cast<uint16_t>(kMsxDskSectorSize);
            state->statusReg = kWd2793St2WriteProtect;
            break;
        }
        case 0xD0u: // Force Interrupt
        default: {
            // Clear any pending operation.
            state->bufPos    = static_cast<uint16_t>(kMsxDskSectorSize);
            state->statusReg = (state->trackReg == 0u) ? kWd2793St1Track0 : 0x00u;
            break;
        }
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
    } else {
        state->sides = 1u;
    }

    state->sectorReg = 1u;
    state->bufPos    = static_cast<uint16_t>(kMsxDskSectorSize);
    state->statusReg = kWd2793St1Track0; // starts at track 0
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
    state->bufPos      = static_cast<uint16_t>(kMsxDskSectorSize);
    state->statusReg   = kWd2793St1Track0;
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
            if (state->bufPos < static_cast<uint16_t>(kMsxDskSectorSize)) {
                const uint8_t byte = state->sectorBuf[state->bufPos++];
                if (state->bufPos >= static_cast<uint16_t>(kMsxDskSectorSize)) {
                    // Sector read complete: clear BUSY and DRQ.
                    state->statusReg = 0x00u;
                }
                return byte;
            }
            return state->dataReg;
        }

        case 0xD4u:
            return state->driveAndSide;

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
            state->trackReg    = value;
            break;
        case 0xD2u:
            state->sectorReg   = value;
            break;
        case 0xD3u:
            state->dataReg     = value;
            break;
        case 0xD4u:
            state->driveAndSide = value;
            break;
        default:
            break;
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
        return false;
    }

    const size_t byteOffset = static_cast<size_t>(logicalSector) * kMsxDskSectorSize;
    if (byteOffset + kMsxDskSectorSize > disk->dskSize) {
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
    const uint8_t count = cpu_b(cpu);
    const uint32_t firstSector = cpu->de;
    const uint16_t dstAddr = cpu->hl;

    if (isWrite || !disk || !disk->dskData) {
        // Writes unsupported (XIP image is read-only); no disk → error.
        set_a(cpu, 0x02u); // write-protect / not ready
        set_carry(cpu);
        return;
    }

    uint8_t sectorBuf[kMsxDskSectorSize];
    for (uint8_t i = 0u; i < count; ++i) {
        if (!msx_disk_read_logical_sector(disk, firstSector + i, sectorBuf)) {
            set_a(cpu, 0x04u); // record not found
            set_carry(cpu);
            return;
        }
        const uint16_t base = static_cast<uint16_t>(dstAddr + static_cast<uint16_t>(i) * kMsxDskSectorSize);
        for (uint16_t b = 0u; b < static_cast<uint16_t>(kMsxDskSectorSize); ++b) {
            msx_memory_write8(memory, static_cast<uint16_t>(base + b), sectorBuf[b]);
        }
    }

    set_b(cpu, 0u);  // all sectors transferred
    clear_carry(cpu);
}

// DSKCHG: Disk-change check.
// Exit: B=1 (not changed), CF=0.
static void patch_dskchg(MsxCpuState* cpu, MsxMemoryState* /*memory*/)
{
    set_b(cpu, 1u);
    clear_carry(cpu);
}

// GETDPB: Build Disk Parameter Block from the disk's boot sector BPB.
// Entry: A=drive, B=media descriptor, HL=address of 21-byte buffer.
// Exit:  (HL) filled, CF=0.
static void patch_getdpb(MsxCpuState* cpu, MsxMemoryState* memory)
{
    MsxDiskState* disk = memory->disk;
    uint8_t boot[kMsxDskSectorSize] = {};

    // Read boot sector (logical sector 0).
    if (disk && disk->dskData && disk->dskSize >= kMsxDskSectorSize) {
        std::memcpy(boot, disk->dskData, kMsxDskSectorSize);
    }

    // Extract BPB fields (FAT12/16 layout starting at offset 0x0B).
    const uint16_t bytesPerSector  = static_cast<uint16_t>(boot[0x0Bu] | (static_cast<uint16_t>(boot[0x0Cu]) << 8));
    const uint8_t  spc             = boot[0x0Du]; // sectors per cluster
    const uint16_t reserved        = static_cast<uint16_t>(boot[0x0Eu] | (static_cast<uint16_t>(boot[0x0Fu]) << 8));
    const uint8_t  numFats         = boot[0x10u];
    const uint16_t rootEntries     = static_cast<uint16_t>(boot[0x11u] | (static_cast<uint16_t>(boot[0x12u]) << 8));
    const uint16_t totalSectors    = static_cast<uint16_t>(boot[0x13u] | (static_cast<uint16_t>(boot[0x14u]) << 8));
    const uint8_t  mediaDesc       = boot[0x15u];
    const uint16_t sectorsPerFat   = static_cast<uint16_t>(boot[0x16u] | (static_cast<uint16_t>(boot[0x17u]) << 8));

    // Use standard 2DD defaults if BPB looks invalid.
    const uint16_t bps  = (bytesPerSector == 512u) ? bytesPerSector : 512u;
    const uint8_t  spc2 = (spc >= 1u && spc <= 8u) ? spc : 2u;
    const uint16_t res  = (reserved >= 1u) ? reserved : 1u;
    const uint8_t  nf   = (numFats >= 1u && numFats <= 2u) ? numFats : 2u;
    const uint16_t re   = (rootEntries > 0u) ? rootEntries : 112u;
    const uint16_t ts   = (totalSectors > 0u) ? totalSectors : 1440u;
    const uint8_t  md   = (mediaDesc >= 0xF0u) ? mediaDesc : 0xF9u;
    const uint16_t spf  = (sectorsPerFat > 0u) ? sectorsPerFat : 3u;

    // Compute derived values.
    const uint16_t fatStart  = res;
    const uint16_t dirStart  = static_cast<uint16_t>(fatStart + static_cast<uint16_t>(nf) * spf);
    const uint16_t dirSectors= static_cast<uint16_t>((static_cast<uint32_t>(re) * 32u + bps - 1u) / bps);
    const uint16_t dataStart = static_cast<uint16_t>(dirStart + dirSectors);
    const uint16_t dataSecs  = static_cast<uint16_t>(ts - dataStart);
    const uint16_t maxCluster= static_cast<uint16_t>(dataSecs / spc2);

    // Cluster mask / shift.
    uint8_t clsMask  = static_cast<uint8_t>(spc2 - 1u);
    uint8_t clsShift = 0u;
    for (uint8_t s = spc2; s > 1u; s >>= 1u) { ++clsShift; }

    // Fill the 21-byte DPB at HL.
    const uint16_t dpbAddr = cpu->hl;
    const uint8_t dpb[21] = {
        md,                                             // 0: media descriptor
        static_cast<uint8_t>(bps & 0xFFu),             // 1: sector size lo
        static_cast<uint8_t>(bps >> 8),                // 2: sector size hi
        clsMask,                                        // 3: cluster mask
        clsShift,                                       // 4: cluster shift
        static_cast<uint8_t>(fatStart & 0xFFu),        // 5: FAT start lo
        static_cast<uint8_t>(fatStart >> 8),            // 6: FAT start hi
        nf,                                             // 7: number of FATs
        static_cast<uint8_t>(re & 0xFFu),              // 8: root dir entries
        static_cast<uint8_t>(dataStart & 0xFFu),       // 9: data sector lo
        static_cast<uint8_t>(dataStart >> 8),           // 10: data sector hi
        static_cast<uint8_t>(maxCluster & 0xFFu),      // 11: max cluster lo
        static_cast<uint8_t>(maxCluster >> 8),          // 12: max cluster hi
        static_cast<uint8_t>(spf & 0xFFu),             // 13: sectors per FAT
        static_cast<uint8_t>(dirStart & 0xFFu),        // 14: dir sector lo
        static_cast<uint8_t>(dirStart >> 8),            // 15: dir sector hi
        0x00u, 0x00u,                                   // 16-17: DTA (unused)
        0x00u,                                          // 18: drive attributes
        0x00u, 0x00u,                                   // 19-20: current dir cluster
    };

    for (uint8_t i = 0u; i < 21u; ++i) {
        msx_memory_write8(memory, static_cast<uint16_t>(dpbAddr + i), dpb[i]);
    }

    clear_carry(cpu);
}

// DSKFMT: Format disk — write-protected, return error.
static void patch_dskfmt(MsxCpuState* cpu, MsxMemoryState* /*memory*/)
{
    set_a(cpu, 0x02u); // write-protected
    set_carry(cpu);
}

// DRVOFF: Drive off — no-op.
static void patch_drvoff(MsxCpuState* cpu, MsxMemoryState* /*memory*/)
{
    clear_carry(cpu);
}

void msx_disk_bios_patch_handler(MsxCpuState* cpu, MsxMemoryState* memory, uint16_t patchAddress)
{
    if (!cpu || !memory) {
        return;
    }

    switch (patchAddress) {
        case 0x4010u: patch_phydio(cpu, memory);          break;
        case 0x4013u: patch_dskchg(cpu, memory);          break;
        case 0x4016u: patch_getdpb(cpu, memory);          break;
        case 0x401Cu: patch_dskfmt(cpu, memory);          break;
        case 0x401Fu: patch_drvoff(cpu, memory);          break;
        default:
            std::printf("[MSX] ED FE at unknown address %04X\n", patchAddress);
            clear_carry(cpu);
            break;
    }
}
