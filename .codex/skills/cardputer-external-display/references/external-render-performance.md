# External Render Performance

External TFT updates are usually SPI-bound. Prefer:

- Batched `writePixels` or DMA transfers
- Multi-line chunks when the core format permits it
- Precomputed RGB565 or RGB444-to-RGB565 expansion tables when memory allows
- Integer-only scaling
- Fit/crop plans computed once per mode change
- External frame pacing independent from core frame pacing

Avoid:

- Per-pixel SPI writes
- Full-screen clears every frame
- Floating-point scaling inside line loops
- External display writes from multiple tasks without ownership
- Unbounded external refresh when audio underflows increase

Benchmark with display average/max time, audio underflows, and heap largest-block values.
