# Migration Checklist

When moving a core from M5 keyboard polling to the direct reader:

- Replace local `M5Cardputer.update()` calls with the shared snapshot update.
- Replace `Keyboard.keysState()` with the direct snapshot.
- Replace `Keyboard.isKeyPressed(c)` with the shared `is_pressed(c)` helper.
- Keep Fn combos working for zoom, volume, brightness, and menu shortcuts.
- Keep `BtnA` long-press quit behavior or provide an exact replacement.
- Confirm `flushInput()` clears both current and previous snapshot state.
- Confirm `waitPress()` still wakes on direct key transitions.
- Test held diagonals and simultaneous A/B/Start/Select.
- Test with and without external I2C joypad detected.

Benchmark input only with logs gated behind an input diagnostics flag. Do not leave per-poll logs enabled in normal builds.
