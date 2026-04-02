#include "msx_disk.h"

#include <cstring>

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
