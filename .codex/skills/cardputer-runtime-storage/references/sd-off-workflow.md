# SD-Off Workflow

This pattern was extracted from SNES, SMS/GG, WS/WSC, and MD branch work:

- SNES closes SD before core init when possible and remounts only for SRAM save/load needs.
- SMS/GG lazily allocates cartridge SRAM and flushes before restart.
- WS/WSC can use SRAM in memory or an SD-backed dirty-page cache, depending on heap pressure.
- MD must avoid assuming SRAM exists; no-SRAM games should not start save tasks.

When centralizing the behavior, use a core allow-list such as `SD_OFF_DURING_GAMEPLAY_CORES`. Do not make it global by default.

The guard should be idempotent. Calling close/remount twice should not corrupt state or hide the real failure.
