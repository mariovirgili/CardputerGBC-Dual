# Config And Keys Reference

Input and config features seen across branches:

- Pause shortcut
- Config menu shortcut
- Key picker shortcut
- Per-core key mapping
- External I2C joypad detection and logging
- Frameskip option in config menu
- FPS or HUD toggles where supported

Use `CardputerInput` for debounced key state and shared input helpers for emulator-facing mappings.

For low-latency keyboard work, prefer a shared direct-reader snapshot layer over calling `M5Cardputer.update()` independently from each core.

For key config changes:

- Keep default mappings stable.
- Save user changes persistently.
- Avoid per-core duplicated picker UI unless the control layouts truly differ.
