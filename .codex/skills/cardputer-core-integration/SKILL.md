---
name: cardputer-core-integration
description: Use when adding, removing, gating, or porting emulator cores in the Cardputer ADV firmware, including CMake source selection, ROM extensions, launch paths, XIP loading, and core feature flags.
---

# Cardputer Core Integration

Use this skill whenever the work touches whether an emulator core exists in a build, how it is launched, or which ROM extensions it exposes.

## Start Here

Inspect these files first:

- `main/CMakeLists.txt`
- `src/main.cpp`
- `src/select_rom.h`
- `src/share/rom_utils.*`
- `src/share/game_save.*`
- Any `src/<core>` folder involved in the change

## Core Gating Pattern

Core inclusion is source-level, not extension-level. A disabled core should behave as if its source tree does not exist:

- Its `src/<core>` sources are not in `APP_SRCS`.
- Its include directories are not in `APP_INCLUDE_DIRS`.
- Its compile definitions, LTO settings, log flags, IRAM toggles, inline toggles, and static buffers are not emitted.
- Its headers are not included from `src/main.cpp`.
- Its launch branch is not compiled.
- Its ROM extensions are hidden from the selector and resolve to `ROM_TYPE_UNKNOWN`.

Mirror the SNES exclusion model. Keep `ENABLE_SNES_CORE` disabled by default unless the user explicitly asks otherwise.

## Launch Checklist

When integrating or changing a core:

1. Add or update the CMake `ENABLE_<CORE>_CORE` switch.
2. Gate all sources, include paths, and core-specific compile options behind that switch.
3. Emit a compile define such as `<CORE>_CORE_ENABLED=1`.
4. Guard `#include` lines and launch branches in `src/main.cpp`.
5. Guard ROM extensions and ROM type detection in `src/select_rom.h`.
6. Add save/load lifecycle support only if the core has SRAM, EEPROM, flash, or state files.
7. Add the core to SD-off gameplay handling only after confirming it does not need live SD access during emulation.

## References

- `references/core-gating.md`
- `references/xip-launch.md`
- `references/core-port-checklist.md`
