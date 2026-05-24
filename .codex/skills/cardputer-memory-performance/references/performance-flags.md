# Performance Flags

Common performance flags include:

- Core enable flags such as `ENABLE_WS_CORE`
- Core LTO flags
- Core logging and benchmark flags
- IRAM placement flags
- Inline fast-path flags
- Tile cache flags
- Tile split flags
- Heap-risk umbrella flags such as `TURN_OFF_IF_HEAP_LOW`

Guideline:

- If a change costs persistent RAM, IRAM, or significant code size, gate it.
- If a change is historical and already part of the stable baseline, leave it outside new heap-risk flags unless directed.
- If a core is disabled, none of its performance flags should matter.
