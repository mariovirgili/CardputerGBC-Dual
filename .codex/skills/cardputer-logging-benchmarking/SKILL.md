---
name: cardputer-logging-benchmarking
description: Use when adding, disabling, or comparing emulator logging, benchmark counters, heap monitoring, audio diagnostics, or per-core CMake log switches in Cardputer ADV.
---

# Cardputer Logging And Benchmarking

Use this skill for diagnostic output, benchmark instrumentation, heap logging, and audio underrun monitoring.

## Start Here

Inspect these files first:

- `main/CMakeLists.txt`
- `src/main.cpp`
- `src/share/emu_log.*`
- Core files that already print `[CORE]`, `[BENCH]`, `[AUD]`, `[AUDX]`, `[HEAP]`, or core-specific tags

## Logging Rules

- Route emulator diagnostics through the project logging macros where available.
- Keep global logging behind master flags such as emulator logs and heap logs.
- Keep core logs behind core-specific CMake flags.
- Put per-source flags in CMake rather than scattering hard-coded `#define` blocks.
- Avoid noisy frame-by-frame logging unless it is rate-limited or benchmark-gated.
- Do not let disabled cores receive log definitions or compile diagnostic code.

## Benchmark Rules

Benchmark counters should be cheap when enabled and absent when disabled:

- Count frames, core time, display time, audio blocks, underflows, and late frames.
- Log aggregate windows, not every frame.
- Keep audio diagnostics separate from core benchmark logs.
- Include heap free, largest internal block, and min-free when heap monitoring is requested.
- Use stable tag names so logs can be compared across builds.

## References

- `references/logging-flags.md`
- `references/benchmark-counters.md`
