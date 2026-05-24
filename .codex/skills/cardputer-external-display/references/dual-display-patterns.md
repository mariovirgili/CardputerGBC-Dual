# Dual Display Patterns

Patterns found in the dual-screen and external-display branches:

- Show ROM file or system information on the external display before launch.
- Keep gameplay on the internal display while showing a static or low-rate system panel externally.
- Support external-only fit modes for cores with unusual resolutions.
- Use independent FPS HUD settings for internal and external display.
- Keep internal zoom and external fit/crop modes separate.
- Add a 30 FPS external lock when external updates threaten audio timing.
- Clear or reinitialize the external panel when switching ROMs or returning to the selector.

Do not assume all cores use the same native resolution or aspect ratio. Put the transform plan next to the core display implementation or in a shared display-target helper.
