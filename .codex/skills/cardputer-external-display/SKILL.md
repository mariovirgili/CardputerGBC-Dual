---
name: cardputer-external-display
description: Use when adding, debugging, or optimizing external TFT, dual-screen, secondary display, ROM info screen, internal/external view mode, or external FPS lock behavior in Cardputer ADV.
---

# Cardputer External Display

Use this skill when work touches an external display, dual-screen rendering, secondary ROM information panels, or different internal/external view modes.

If the change involves zoom scrolling or auto-follow crop behavior, use the `cardputer-zoom-scroll-follow` skill as well.

## Start Here

Inspect these files first:

- `src/cardputer/CardputerView.*`
- `src/main.cpp`
- The target core display file, such as `src/msx/msx_display.cpp`, `src/gx4000/gx4000_display.cpp`, `src/sms/display.cpp`, `src/gbc/gbc_display.cpp`, `src/ws/ws_display.cpp`, or `src/genesis/genesis_display.cpp`
- Core input files that handle `CARDPUTER_SCREEN_TOGGLE`
- Display-related CMake flags in `main/CMakeLists.txt`

If the change is based on the historical dual-screen implementation, inspect `D:/CardputerADV/Compile/CardputerGBC-Dual` branches such as `feature/dual-screen-adv`, `feature/finaldual`, `msx1-dualcore`, `msx2-dualcore`, `ColecoIntegration`, and `8-bitGoldiesFinal`.

## External Display Rules

- Keep external display support behind a build flag or per-core feature flag.
- The internal Cardputer display must remain a working fallback.
- Treat internal and external view modes independently: fit, crop, zoom, fullscreen, HUD, and FPS lock can differ.
- Avoid blocking the emulator core on slow external SPI transfers.
- Prefer batched line or tile transfers over per-pixel writes.
- Use a fixed external FPS cap, such as 30 FPS, when full-rate external updates hurt audio timing.
- Keep ROM info or system info screens separate from gameplay frame pushing.
- Do not allocate external framebuffers unless the selected core and flag require them.

## Integration Pattern

Use a small display routing layer instead of scattering external-display checks across every renderer:

- `display_target_init(core, mode)`
- `display_target_has_external()`
- `display_target_push_internal(...)`
- `display_target_push_external(...)`
- `display_target_present_info(...)`
- `display_target_shutdown()`

Core renderers should produce a stable RGB565 or indexed line/tile buffer, then hand it to the routing layer.

## References

- `references/dual-display-patterns.md`
- `references/external-render-performance.md`
- `references/external-display-checklist.md`
