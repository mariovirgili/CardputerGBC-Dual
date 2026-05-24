# Logging Flags

Common flag categories:

- Master emulator logging
- Heap monitoring
- Audio diagnostics
- Core benchmark logs
- Core-specific trace logs

Expected CMake behavior:

- OFF means no compile definition for that feature.
- Core-specific logs are only applied when that core is enabled.
- Per-source logging flags should use source properties or a helper such as `add_source_compile_defs`.
- Do not enable logs for excluded source files.

Useful tags seen across branches:

- `[BOOT]`
- `[HEAP]`
- `[ROM]`
- `[FLASH]`
- `[XIP]`
- `[CORE]`
- `[SAVE]`
- `[BENCH]`
- `[AUD]`
- `[AUDX]`
- Core tags such as `[WS]`, `[GEN]`, `[SMS]`, `[SNES]`, `[NGP]`, `[MSX]`
