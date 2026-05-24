# Hot Path Patterns

Patterns seen in renderer and audio work:

- Skip empty tile rows before preparing pixel pointers.
- Cache decoded tile or sprite metadata only when the memory cost is acceptable.
- Split tile paths when it removes repeated conditionals in the inner loop.
- Avoid per-pixel branches in fixed-width loops when the fallback remains exact.
- Use dirty-page tracking for SRAM instead of writing full memory repeatedly.
- Use direct audio paths only with underrun diagnostics enabled during tuning.

For any hot path, keep the old exact path available behind a flag or fallback until logs show the new path is stable.
