# Benchmark Counter Set

Use a small, consistent counter set:

- Core frame count
- Rendered or displayed frame count
- Average and max core frame time
- Average and max display time
- Late frame count and max lateness
- Frameskip level
- Audio blocks produced
- Audio underflows
- Audio queue availability min, average, max
- Heap free
- Largest internal allocation block
- Minimum free heap

For renderer-heavy cores, include domain counters only when useful:

- Sprite count and visible pixels
- Tile cache hits or misses
- DMA or GDMA transfers
- IRQ counts

Avoid adding counters that require large permanent arrays unless the user explicitly accepts the RAM cost.
