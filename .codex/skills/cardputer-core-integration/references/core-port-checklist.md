# Core Port Checklist

Before considering a core integrated, verify:

- ROM extensions are visible only when the core is enabled.
- Disabled extensions return unsupported.
- Launch code compiles out when the core is disabled.
- Save paths are unique and stable.
- SRAM is loaded before gameplay and flushed on exit.
- Audio and display helper tasks use the intended core affinity.
- Core-specific debug logs are behind CMake flags.
- Optional optimizations are behind core or heap-risk flags.
- No core-owned static framebuffer, tile cache, palette cache, or IRAM table remains when the core is disabled.

Do not compile unless the user asks for it.
