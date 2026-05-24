# Manual Scroll Controls

Existing shared key constants:

- `CARDPUTER_ZOOM_PLUS` is `Fn + /`, the right-arrow key
- `CARDPUTER_ZOOM_MINUS` is `Fn + ,`, the left-arrow key
- Direction aliases also exist for left, right, up, and down

Recommended behavior:

- `Fn + Right`: increase zoom percent or zoom level.
- `Fn + Left`: decrease zoom percent or zoom level.
- `Fn + Up`: pan viewport up when zoomed.
- `Fn + Down`: pan viewport down when zoomed.
- `Fn + Left/Right`: if zoom is unchanged by repeat policy, pan horizontally.

Because `Fn + Left/Right` can mean both zoom and pan, pick one policy per core:

- Simple policy: left/right adjust zoom, up/down pan vertically.
- Advanced policy: short press adjusts zoom, hold scrolls viewport.
- Split policy: separate zoom keys and scroll keys in the config menu.

Always clamp offsets so the source ROI remains inside the native frame.
