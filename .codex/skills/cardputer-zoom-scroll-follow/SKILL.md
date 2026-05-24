---
name: cardputer-zoom-scroll-follow
description: Use when adding or debugging manual zoom scrolling with Fn plus arrow keys, zoom/crop view modes, or adaptive auto-follow that tracks on-screen action while zoomed.
---

# Cardputer Zoom Scroll Follow

Use this skill when a core needs zoom controls beyond a simple fullscreen toggle: manual viewport scrolling, crop panning, or auto-follow while zoomed.

## Start Here

Inspect these files first:

- `src/share/input.h`
- The target core input file
- The target core display file
- Any core config file that stores view mode or zoom percent
- `src/cardputer/CardputerInput.*` if the behavior depends on direct key transitions

Useful existing examples include:

- `src/gbc/gbc_input.cpp` and `src/gbc/gbc_display.cpp`
- `src/pce/pce_input.cpp` and `src/pce/pce_display.*`
- `src/atari2600/a2600_input.cpp` and `src/atari2600/a2600_display.cpp`
- `src/atari7800/a7800_input.cpp` and `src/atari7800/a7800_video.cpp`

Historical branch references in `D:/CardputerADV/Compile/CardputerGBC-Dual` include commits named "Added Scroll View (Fn + arrows)" and "Added Auto follow on Scroll View while zooming".

## Control Rules

- Use `Fn + Right` and `Fn + Left` for zoom in/out when matching the existing project behavior.
- Use `Fn + Up/Down/Left/Right` for viewport panning only while a zoomed/crop view is active.
- Do not let zoom panning consume normal emulator D-pad input unless `Fn` is held.
- Clamp zoom percent and viewport offsets every frame.
- Keep a fast reset path back to fit/fullscreen.
- Persist zoom mode only if the core already persists display settings.

## Adaptive Follow Rules

Auto-follow should move the crop window toward visible action without jitter:

- Track a low-cost activity bounding box from sprites, changed pixels, player position, or non-background pixels.
- Smooth the viewport target over multiple frames.
- Use a dead zone so small movements do not constantly pan.
- Temporarily prefer manual scroll after the user presses `Fn + arrows`.
- Fall back to centered crop when activity cannot be detected.
- Keep all follow state in the display module, not in CPU or audio hot paths.

## References

- `references/manual-scroll-controls.md`
- `references/adaptive-follow.md`
- `references/viewport-math.md`
