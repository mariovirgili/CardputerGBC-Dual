---
name: cardputer-runtime-storage
description: Use when changing SD-card lifecycle, XIP gameplay, SRAM or save-state persistence, dirty-page saves, or remount-on-exit behavior for Cardputer ADV emulator cores.
---

# Cardputer Runtime Storage

Use this skill for SD-off-during-gameplay, save handling, SRAM flushing, and shutdown/restart cleanup.

## Start Here

Inspect these files first:

- `src/share/sd_gameplay_guard.*`
- `src/share/sd_control.*`
- `src/share/game_save.*`
- `src/main.cpp`
- The core save implementation, such as `src/ws/ws_save.*`, `src/genesis/genesis_save.*`, `src/sms/.../save.*`, or `src/gbc/...`

## SD-Off Gameplay Pattern

The safe pattern is:

1. Load ROM from SD and map it through XIP.
2. Load SRAM or state data before closing SD.
3. Close SD only after all required load-time reads are complete.
4. Run the emulator without SD traffic.
5. On quit, defer restart long enough for the core to exit cleanly.
6. Snapshot or flush SRAM while core-owned memory is still valid.
7. Remount SD only for the final save write.
8. Close or release core buffers after persistence is complete.
9. Restart or return to the selector.

If a core uses SD-backed live SRAM pages, do not close SD during gameplay unless the cache can absorb all writes and flush dirty pages later.

## Save Rules

- Prefer dirty-page saves when SRAM can be large or frequently touched.
- Avoid blocking SD writes from audio, display, or timing-sensitive paths.
- Preserve save compatibility with existing filenames.
- Log save failures clearly, but keep noisy save tracing behind core log flags.
- If a game has no SRAM, skip save tasks and avoid allocating save buffers.

## References

- `references/sd-off-workflow.md`
- `references/save-system-patterns.md`
