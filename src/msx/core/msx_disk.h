#pragma once

#include <stddef.h>
#include <stdint.h>

// WD2793 floppy disk controller emulation for MSX DSK images.
// Uses a DRQ-polling model (no interrupt line); data-ready bit is in the status register.
//
// Supported DSK formats:
//   1DD  80 tracks × 1 side  × 9 sectors × 512 B = 368 640 B
//   2DD  80 tracks × 2 sides × 9 sectors × 512 B = 737 280 B
//
// I/O port map (MSX standard):
//   0xD0  W = command register      R = status register
//   0xD1  W/R = track register
//   0xD2  W/R = sector register  (1-based)
//   0xD3  W/R = data register    (data stream during sector read)
//   0xD4  W/R = drive/side select (bit 0 = side, bits 1-2 = drive number)

constexpr size_t  kMsxDskSectorSize       = 512u;
constexpr uint8_t kMsxDskSectorsPerTrack  = 9u;
constexpr uint8_t kMsxDskTracks           = 80u;

// Status register bits – Type 1 commands (Restore / Seek)
constexpr uint8_t kWd2793St1Track0        = 0x04u;
constexpr uint8_t kWd2793St1Busy          = 0x01u;

// Status register bits – Type 2 commands (Read / Write Sector)
constexpr uint8_t kWd2793St2WriteProtect  = 0x40u;
constexpr uint8_t kWd2793St2RecordNotFound = 0x10u;
constexpr uint8_t kWd2793St2Drq           = 0x02u;
constexpr uint8_t kWd2793St2Busy          = 0x01u;

struct MsxDiskState {
    const uint8_t* dskData;      // DSK image pointer (XIP-mapped; may be nullptr = no disk)
    size_t         dskSize;      // total DSK image size in bytes
    uint8_t        sides;        // 1 or 2 (determined from image size)

    uint8_t  trackReg;           // WD2793 track register
    uint8_t  sectorReg;          // WD2793 sector register (1-based)
    uint8_t  dataReg;            // WD2793 data register (seek target / write data)
    uint8_t  statusReg;          // WD2793 status register
    uint8_t  driveAndSide;       // port 0xD4: bit 0 = side, bits 1-2 = drive number

    uint8_t  sectorBuf[kMsxDskSectorSize];
    uint16_t bufPos;             // read offset in sectorBuf; >= kMsxDskSectorSize means no data pending
    bool     ready;              // disk state initialised
};

void    msx_disk_init(MsxDiskState* state, const uint8_t* dskData, size_t dskSize);
void    msx_disk_reset(MsxDiskState* state);
uint8_t msx_disk_in(MsxDiskState* state, uint8_t port);
void    msx_disk_out(MsxDiskState* state, uint8_t port, uint8_t value);
bool    msx_disk_read_logical_sector(const MsxDiskState* state, uint32_t logicalSector, uint8_t* dest);

// Apply ED FE C9 patches to the DISK ROM buffer at the standard BIOS entry points
// (PHYDIO=0x4010, DSKCHG=0x4013, GETDPB=0x4016, DSKFMT=0x401C, DRVOFF=0x401F).
// diskRomBuf must be writable and at least 0x20 bytes long (disk ROM starts at 0x4000).
void    msx_disk_apply_rom_patches(uint8_t* diskRomBuf, size_t diskRomSize);

// BIOS software-trap handler invoked by the CPU when it executes ED FE.
// Handles PHYDIO, DSKCHG, GETDPB, DSKFMT, DRVOFF without touching hardware registers.
struct MsxCpuState;
struct MsxMemoryState;
void    msx_disk_bios_patch_handler(struct MsxCpuState* cpu,
                                    struct MsxMemoryState* memory,
                                    uint16_t patchAddress);
