---
name: cardputer-ui-input-config
description: Use when working on the Cardputer ROM selector, config menu, key picker, key mapping, input shortcuts, directory indexes, cursor restore, or menu navigation.
---

# Cardputer UI Input Config

Use this skill for launcher UI, ROM selector behavior, config menus, key mapping, and input handling.

If the work is specifically about bypassing `M5Cardputer.Keyboard` with a lower-level keyboard scanner, use the `cardputer-direct-keyboard` skill as well.

If the work changes secondary or external display behavior, use the `cardputer-external-display` skill as well.

If the work changes zoom keys, viewport scrolling, or action-following crop behavior, use the `cardputer-zoom-scroll-follow` skill as well.

## Start Here

Inspect these files first:

- `src/select_rom.h`
- `src/cardputer/VerticalSelector.*`
- `src/cardputer/CardputerInput.*`
- `src/cardputer/CardputerView.*`
- `src/cardputer/SdService.*`
- `src/share/input.*`
- Core-specific key mapping or config files

## ROM Selector Rules

- Show only extensions for enabled cores.
- Keep search and paging responsive on SD cards with many files.
- Preserve cursor position where possible.
- Use directory index/cache files when available, but do not make them mandatory.
- Treat unsupported extensions as `ROM_TYPE_UNKNOWN`.
- Return to the selector cleanly after unsupported or oversized ROM errors.

## Config Menu Rules

- Use existing menu primitives instead of creating a separate UI style.
- Keep config actions short and reversible.
- Use the shared input abstraction for Cardputer keys and external I2C joypads.
- Keep key picker behavior consistent across cores.
- Store key config in the existing settings path or NVS mechanism used by the project.

## References

- `references/rom-selector.md`
- `references/config-and-keys.md`
