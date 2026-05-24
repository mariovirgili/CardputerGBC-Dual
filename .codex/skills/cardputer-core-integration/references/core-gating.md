# Core Gating Reference

Use one CMake option per emulator family:

- `ENABLE_NES_CORE`
- `ENABLE_SMS_CORE` for SMS, Game Gear, SG-1000, and ColecoVision
- `ENABLE_NGP_CORE`
- `ENABLE_MD_CORE`
- `ENABLE_WS_CORE`
- `ENABLE_PCE_CORE`
- `ENABLE_GB_CORE` for GB and GBC
- `ENABLE_LYNX_CORE`
- `ENABLE_MSX_CORE`
- `ENABLE_A7800_CORE`
- `ENABLE_A2600_CORE`
- `ENABLE_GX4000_CORE`
- `ENABLE_SNES_CORE`

Filtering must remove more than just `.c` or `.cpp` files. Check for:

- Core source folders in `APP_SRCS`
- Extra library folders, for example NES nofrendo sources
- Include directories
- Per-source compile definitions
- Per-source optimization or LTO properties
- IRAM and inline feature defines
- Static tables or generated databases
- Log and benchmark compile flags

Do not leave disabled core declarations in shared launch code if they cause references to symbols in excluded sources.
