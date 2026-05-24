# External Display Checklist

When implementing external display support:

- Add a build flag and keep default behavior clear.
- Detect external display availability during hardware init.
- Leave `cfg.external_display_value` choices intentional in `src/main.cpp`.
- Initialize the external panel once, not inside every frame.
- Keep byte order and `setSwapBytes` correct for both internal and external targets.
- Decide whether the external display shows gameplay, ROM info, HUD, or system status.
- Ensure quit/restart tears down external display state cleanly.
- Verify selector return path after unsupported ROMs.
- Validate with audio diagnostics because display stalls often show up as underruns.
- Keep any external framebuffer or conversion table out of disabled-core builds.
