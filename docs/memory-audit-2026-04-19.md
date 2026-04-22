## Memory audit for `m5stack-stamps3-max-spiffs`

Date: `2026-04-19`

Note:

- this document started as a baseline snapshot of the build before the demand-driven refactors
- after that baseline, `DISK.ROM`, the PSG ring buffer, and the MSX static pool were moved away from permanent boot-time DRAM reservation

This audit is based on the current build artifact:

- `.pio/build/m5stack-stamps3-max-spiffs/firmware.elf`

### DRAM sections

From `xtensa-esp32s3-elf-size -A firmware.elf`:

- `.dram0.data` = `13780` bytes
- `.dram0.bss` = `104480` bytes

These sections reduce the free heap available before the emulator starts.

### Largest fixed DRAM allocations currently linked

From `xtensa-esp32s3-elf-nm -S --size-sort firmware.elf`:

| Size | Symbol | Purpose |
| --- | --- | --- |
| `57344` | `g_emu_static_pool` | Shared static emulation pool for MSX banking / reserved BIOS-related storage |
| `16384` | `s_msx_disk_rom_static` | Permanent fallback buffer for `DISK.ROM` |
| `8192` | `s_openBusPage` | Open-bus page filled with `0xFF` |
| `8192` | `s_psgRing` | PSG audio ring buffer |
| `2048` | `s_mixBuffer` | Audio mix buffer |
| `1024` | `s_palettePairs565` | Cached palette-pair conversions |
| `728` | `s_vdpStateSnapshot` | Snapshot used by MSX VDP render path |
| `320` | `s_msxColorSpriteLine` | Per-line color sprite scratch |
| `256` | `s_msx2LineBuffer` | MSX2 line streaming scratch |
| `256` | `s_msxSpriteOccupancy` | Sprite occupancy scratch |

The top four alone account for `90112` bytes of fixed DRAM.

### What is already demand-driven

Some large MSX video buffers are already allocated only when needed:

- `s_msxFrameBuffer`
- `s_msx1Vram`
- `s_msx1VramB`

These live in:

- `src/msx/core/msx_vdp.cpp`

This proves that the firmware already supports the "allocate on demand" pattern.

### Main conclusion

The Reddit comment is directionally correct:

- the firmware is indeed reserving a meaningful amount of DRAM before emulation starts
- the biggest issue is not just early heap allocation, but also large permanent global buffers in `.bss`

### Safe improvements identified

The lowest-risk fixed DRAM reductions are:

1. Make `DISK.ROM` storage demand-driven instead of a permanent `16 KB` static buffer.
2. Make the PSG ring buffer demand-driven instead of a permanent `8 KB` static buffer.

Expected fixed DRAM reduction from these two changes alone:

- `16384 + 8192 = 24576` bytes

### More complex future candidate

`g_emu_static_pool` (`56 KB`) is the largest fixed reservation.

It is probably the next big target, but it is also more delicate because it influences:

- mapper memory
- reserved BIOS/static page placement
- contiguous memory guarantees during core init

That one should be approached as a dedicated refactor, not as a quick cleanup.
