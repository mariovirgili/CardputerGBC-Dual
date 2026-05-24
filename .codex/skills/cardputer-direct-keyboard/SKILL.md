---
name: cardputer-direct-keyboard
description: Use when implementing or debugging a direct Cardputer keyboard reader that bypasses M5Cardputer Keyboard polling, input latency, missed key states, matrix scanning, debounce, or Fn/key-combo behavior.
---

# Cardputer Direct Keyboard

Use this skill when the M5 keyboard abstraction is too slow, misses transitions, adds unwanted debounce, or conflicts with emulator timing.

## Start Here

Inspect these files first:

- `src/cardputer/CardputerInput.*`
- `src/share/input.*`
- `src/compat/i2c_bus.*`
- Core input files such as `src/gbc/gbc_input.cpp`, `src/sms/input.cpp`, `src/ws/ws_input.cpp`, `src/genesis/genesis_input.cpp`
- The M5Cardputer library keyboard driver under `lib/M5Cardputer` if the direct reader must match its matrix protocol

## Direct Reader Rules

- Keep the direct reader behind a compile flag such as `ENABLE_DIRECT_KEYBOARD_READER`.
- Keep a M5 fallback path available until the direct reader is proven stable.
- Do not call `M5Cardputer.update()` from hot emulator input loops when the direct path is enabled.
- Poll at a bounded cadence; do not busy-loop the keyboard bus every CPU slice.
- Preserve shared key constants from `src/share/input.h`.
- Keep common actions such as quit, volume, brightness, screen toggle, and Fn combos equivalent to the M5 path.
- Treat external I2C joypad polling separately from the internal keyboard scanner.

## Integration Pattern

Expose one shared snapshot API rather than copying matrix reads into each core:

- `cardputer_keyboard_update()`
- `cardputer_keyboard_snapshot()`
- `cardputer_keyboard_is_pressed(char key)`
- `cardputer_keyboard_fn_pressed()`
- `cardputer_keyboard_changed()`

Core input files should read the shared snapshot and then map it to emulator buttons.

## References

- `references/direct-reader-design.md`
- `references/migration-checklist.md`
