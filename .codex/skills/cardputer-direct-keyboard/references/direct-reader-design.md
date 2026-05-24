# Direct Reader Design

The current input paths often use:

- `M5Cardputer.update()`
- `M5Cardputer.Keyboard.keysState()`
- `M5Cardputer.Keyboard.isKeyPressed(...)`
- `M5Cardputer.Keyboard.isChange()`

A direct keyboard reader should replace those calls with a single low-latency snapshot layer.

Recommended shape:

1. Scan the internal keyboard hardware once per input tick.
2. Store raw pressed state, previous state, changed mask, and Fn state.
3. Convert raw matrix positions to the existing project key constants.
4. Let `CardputerInput` and core input files consume the same snapshot.
5. Keep the M5 implementation as a compile-time fallback.

Avoid per-core direct scanning. It duplicates debounce policy and can produce inconsistent held-key behavior across cores.

The direct reader must not take over the external I2C joypad bus. Internal keyboard scan and external joypad scan are separate concerns.
