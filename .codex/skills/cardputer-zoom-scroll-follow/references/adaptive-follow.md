# Adaptive Follow

Adaptive follow keeps a zoomed viewport centered on the action.

Possible activity sources:

- Sprite bounding boxes
- Dirty rectangle from changed pixels
- Non-background pixel bounds
- Player/object coordinates if the core exposes them cheaply
- Tilemap scroll registers for systems with hardware scrolling

Smoothing pattern:

1. Compute `targetX` and `targetY` from the activity center.
2. Apply a dead zone around the current viewport center.
3. Move only partway to the target each frame.
4. Clamp to source frame bounds.
5. Disable or dampen follow for a short cooldown after manual scroll.

Avoid expensive full-frame scans on every frame. If a scan is necessary, sample rows or reuse renderer metadata that already exists.
