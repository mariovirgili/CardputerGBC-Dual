---
name: cardputer-memory-performance
description: Use when optimizing Cardputer ADV cores with IRAM, inline fast paths, tile caches, tile splitting, LTO, heap-risk flags, lazy buffers, or renderer hot paths.
---

# Cardputer Memory Performance

Use this skill for performance changes that can affect heap, IRAM, code size, or static RAM.

If the performance work is specifically about an external TFT or dual-screen path, use the `cardputer-external-display` skill as well.

If the performance work is about zoomed viewport panning or adaptive follow, use the `cardputer-zoom-scroll-follow` skill as well.

## Start Here

Inspect these files first:

- `main/CMakeLists.txt`
- The target core renderer, CPU, APU, or mapper files
- Existing heap and benchmark logs
- Any core-specific config header

## Optimization Rules

- Put new RAM, IRAM, inline, tile-cache, and tile-split changes behind CMake flags.
- Respect global heap-risk switches such as `TURN_OFF_IF_HEAP_LOW`: ON keeps risky optimizations enabled, OFF disables the ones that cost RAM or IRAM.
- Do not move historical IRAM annotations under new flags unless the user explicitly asks.
- Prefer lazy or post-SD-close allocation for large buffers.
- Prefer source-specific optimization or LTO over global build changes.
- Measure with benchmark and heap logs before claiming a win.
- Preserve emulation accuracy first; fast paths must have an exact fallback.

## Hot Path Checklist

Before changing a renderer or APU loop:

1. Confirm the bottleneck with logs or counters.
2. Estimate RAM, IRAM, and flash cost.
3. Add a feature flag if the change increases memory pressure.
4. Keep benchmark counters comparable before and after.
5. Verify disabled-core builds do not compile the optimized source.

## References

- `references/performance-flags.md`
- `references/hotpath-patterns.md`
