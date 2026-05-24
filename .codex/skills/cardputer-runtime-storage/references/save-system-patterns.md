# Save System Patterns

Use these save patterns:

- `No save`: print one concise diagnostic and do not allocate a save buffer.
- `Small SRAM`: allocate once, load at startup, flush all on exit.
- `Large SRAM`: dirty-page tracking, final flush of touched pages.
- `SD-backed cache`: use only when heap cannot hold the full SRAM; guard dummy pages and keep page ownership clear.
- `Save task`: only start when the core can safely hand off writes without racing core memory.

Exit order matters. The save code often needs core-owned SRAM pointers, so do not free the core before final persistence unless a snapshot was taken.
