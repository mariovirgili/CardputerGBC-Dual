# Msx ADV Emulators v0.5

![CardputerGBC-Dual external title screen](images/Externaltitle.png)

Msx ADV Emulators v0.5 is an MSX-focused firmware for the M5Stack Cardputer
with support for internal LCD play and optional external SPI TFT output.

This release is built for the `m5stack-stamps3-max-spiffs-msx2-flash`
environment. It keeps the official MSX2 BIOS files out of the firmware image:
on first MSX2 use, the firmware can validate `MSX2.ROM` and `MSX2EXT.ROM` from
the SD card and cache them into a dedicated flash partition.

MSX is a registered trademark owned by MSX Licensing Corporation.

## v0.5 Highlights

- MSX1 and MSX2 runtime on ESP32-S3 without PSRAM.
- Flash-backed MSX2 BIOS cache partition for `MSX2.ROM` and `MSX2EXT.ROM`.
- Embedded C-BIOS fallback for MSX1 cartridge loading.
- `.rom`, `.dsk`, and `.cas` launch support.
- CAS runtime menu with `RUN"CAS:"`, `BLOAD"CAS:",R`, and CAS change actions.
- Cartridge mapper support for plain ROMs, ASCII, and Konami-style cartridges.
- MSX1 VDP and MSX2 V9938-oriented video paths, including MSX2 bitmap modes.
- Save states with selectable slots and quick save/load shortcuts.
- Runtime configuration menu, startup configuration shortcuts, and editable keys.
- Internal LCD or external SPI TFT output, with saved view settings.
- External-screen helper overlay on the internal LCD showing ROM title and keys.
- Virtual key picker for entering MSX keyboard characters while in joy/key modes.
- Hidden About-page easter egg with an input diagnostic tester.

## Two-Week Polish Pass

v0.5 also includes a large number of small fixes and optimizations that are easy
to miss in a short feature list, but make the firmware feel much more solid on
real Cardputer hardware.

Boot and media fixes:

- MSX BIOS boot path refined for small cartridges, including 16 KB titles that
  rely on the BIOS cartridge search and slot handoff.
- More accurate startup slot/work-area setup for BIOS-driven ROM launch.
- Disk and cassette paths kept separate from cartridge direct-boot behavior.
- CAS BASIC startup refresh improved so the BASIC cursor screen appears without
  waiting for extra input.
- CAS change flow can return to the correct static/game display state after a
  tape swap.

Display and performance fixes:

- Internal and external display paths are treated independently, avoiding
  unnecessary redraw work on the screen that is not running the game.
- When playing on the external TFT, the internal LCD shows the ROM title and
  controls once, then stops refreshing.
- When playing on the internal LCD, the external TFT now shows a static MSX1 or
  MSX2 title panel plus the current game name, then stops refreshing.
- External 30 fps mode, frameskip choices, FPS HUD, and MSX2 render toggles are
  exposed as runtime performance controls.
- Runtime overlays, CAS selector, About page, and input tester received redraw
  throttling and centering fixes to reduce flicker.
- View changes request an immediate redraw instead of waiting for another input
  event.

Input and control fixes:

- `Config Keys` no longer treats navigation keys as automatic row shortcuts.
- Current key bindings are shown next to each configurable action.
- `JOY EXTEND`, `KEYB/JOY`, `BasicKeyboard`, and `Vaus` behavior was separated
  more clearly, including `BasicKeyboard` disabling incompatible modes.
- Control labels were renamed to match their actual behavior.
- `START` and `MENU` mappings can be used as MSX keyboard helpers where useful.
- Brightness and volume shortcuts were clarified, with brightness applying to
  the internal LCD.
- Runtime input diagnostic easter egg helps verify raw Cardputer keys, emulator
  actions, joystick output, keyboard matrix output, and Vaus state.

Virtual keyboard and CAS macro fixes:

- `V` opens the virtual key picker in joy/key modes without also injecting Enter.
- Picker navigation has debounce and controlled key repeat for left/right hold.
- Picker redraw is throttled to avoid excessive flicker.
- SPACE and ENTER are available from the picker as `_` and `E`.
- `RUN CAS`, `BLOAD CAS`, `Fn + C`, and `Fn + B` type their macro and press
  Enter automatically.
- After CAS macros, temporary control overrides are restored to the saved
  control configuration.
- Returning from CAS macro selection resets the runtime menu selector to the
  first row, avoiding stale menu focus.

## Release Binary

The v0.5 flashable image is:

```text
release/MsxADV-Emulators-v0.5-m5stack-stamps3-max-spiffs-msx2-flash-flashable.bin
```

SHA-256:

```text
21309A1389CDC1A7040CCB2824AF060D79073351FD0B94954D72BB55380AAF2C
```

Flash it at offset `0x0`:

```powershell
C:\Users\user\.platformio\penv\Scripts\python.exe -X utf8 C:\Users\user\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32s3 --port COM4 --baud 921600 write_flash 0x0 release\MsxADV-Emulators-v0.5-m5stack-stamps3-max-spiffs-msx2-flash-flashable.bin
```

## Supported Media

| Type | Extensions | Notes |
| --- | --- | --- |
| MSX cartridge ROM | `.rom` | 8 KB aligned cartridge images. |
| MSX disk image | `.dsk` | Uses the MSX disk path and Disk ROM support. |
| MSX cassette image | `.cas` | Can boot to BASIC, then use CAS macros. |

ROM files must be uncompressed. Do not use `.zip`, `.7z`, or `.rar`.

## SD Card Layout

Suggested folders:

```text
/sd/roms/MSX/
/sd/bios/private/
/sd/bios/msx/
/sd/msx/
```

For the default v0.5 flash build, place legally obtained MSX2 BIOS files here
before first MSX2 launch:

```text
/sd/bios/private/MSX2.ROM
/sd/bios/private/MSX2EXT.ROM
```

The firmware validates them and writes them into the `msx2bios` flash
partition. After that, MSX2 can start without loading those two files into RAM.

Other useful BIOS files:

```text
/sd/bios/msx/MSX.ROM
/sd/bios/msx/DISK.ROM
/sd/bios/msx/MSXDOS2.ROM
/sd/bios/msx/FMPAC.ROM
```

Reference hashes shown by the firmware:

```text
MSX.ROM      364a1a579fe5cb8dba54519bcfcdac0d
DISK.ROM     80dcd1ad1a4cf65d64b7ba10504e8190
MSXDOS2.ROM  6418d091cd6907bbcf940324339e43bb
FMPAC.ROM    6f69cc8b5ed761b03afd78000dfb0e19
```

## Startup Flow

At boot, the firmware can offer to resume the last ROM. If you do not resume,
the startup menu contains:

- `Rom selector`
- `Config Menu`
- `Config Keys`
- `About`

The selected startup menu item is saved and restored next time.

Holding `G0` during boot provides boot options:

- hold about 900 ms: force ROM selector and skip auto launch
- hold longer: reset saved ROM history and browser path

## ROM Selector

The ROM selector supports folders, filtering, and cached directory indexes.

Controls:

- `E` = up
- `Z` = down
- `A` = jump backward by 4 entries
- `D` = jump forward by 4 entries
- hold `A` or `D` = fast repeat scrolling
- `P` or `Enter` = open folder or select file
- `K` = parent folder or back
- type letters/numbers = filter list
- `Del` = delete filter character
- long-press `G0` = selector options, including index refresh

On the `RESUME LAST GAME?` prompt:

- `D`, `Right`, or `Enter` = yes
- `A`, `Left`, or `G0` = no

## Config Menu

The startup `Config Menu` and the in-game MSX menu share the same runtime
settings where applicable.

Main options:

- `Performance`: opens performance tuning.
- `JOY EXTEND`: enables joystick plus extra MSX keyboard helper mappings.
- `KEYB/JOY`: maps configured controls to MSX keyboard actions while keeping
  keyboard input available.
- `BasicKeyboard`: direct Cardputer keyboard to MSX keyboard mode. When enabled,
  joystick and Vaus are disabled.
- `Vaus`: Arkanoid-style Vaus input mode.
- `View`: cycles the active view mode for the selected display.
- `StateSlot`: selects save slot `0` to `9`.
- `SaveState`: writes the selected save slot.
- `LoadState`: loads the selected save slot.
- `CAS MENU`: appears only when a `.cas` is loaded.
- `Close`: returns to the emulator.

Performance options:

- `EXT 30FPS`: fixed 30 fps external display mode.
- `FRAMESKP`: frameskip mode, including adaptive and fixed ratios.
- `FPS HUD`: show or hide frame statistics.
- `SLICE RENDER`: MSX2 rendering strategy toggle.
- `SPR COLL`: sprite collision behavior toggle.
- `8-SPR FLAGS`: sprite overflow simplification.
- `INSTANT CMD`: V9938 command timing simplification.
- `BACK`: return to the main menu.

CAS menu options:

- `RUN CAS`: closes the menu and types `RUN"CAS:"` followed by Enter.
- `BLOAD CAS`: closes the menu and types `BLOAD"CAS:",R` followed by Enter.
- `CHANGE CAS`: opens a CAS selector and swaps the current tape.
- `BACK`: return to the main menu.

## Config Keys

`Config Keys` edits the MSX control bindings and stores them on SD.

Default bindings:

| Action | Default key |
| --- | --- |
| `UP` | `E` |
| `DOWN` | `S` |
| `LEFT` | `A` |
| `RIGHT` | `D` |
| `PRIMARY` | `L` |
| `SECONDARY` | `K` |
| `START` | `1` |
| `MENU` | `2` |

The editor shows the current key next to each action. Move to an action and
confirm to capture a new key. `SAVE` writes the config, `DEFAULTS` restores the
defaults, and `CANCEL` returns without saving.

`INT VIEW` in the same editor switches the internal view style.

## In-Game Controls

Global runtime controls:

- short `G0` = quit safely and return to the ROM selector
- long `G0` = open or close the runtime menu
- long backtick = quit safely and return to the ROM selector
- `Fn + S` = quick save to the selected slot
- `Fn + L` = quick load from the selected slot
- `Fn + C` = type `RUN"CAS:"` plus Enter
- `Fn + B` = type `BLOAD"CAS:",R` plus Enter
- `Fn + =` or `Fn + +` = volume up
- `Fn + -` or `Fn + _` = volume down
- `Fn + ]` or `Fn + }` = internal LCD brightness up
- `Fn + [` or `Fn + {` = internal LCD brightness down
- `\` = change view
- `Fn + ,` / `Fn + /` = zoom controls where supported

MSX keyboard helpers:

- `Fn + 1` to `Fn + 5` = MSX `F1` to `F5`
- `Fn + Tab` = `STOP`
- `Fn + Del` = `DEL`
- `Fn + Enter` = `SELECT`
- `Fn + Space` = `HOME`
- `Fn + ,` = cursor left
- `Fn + .` = cursor down
- `Fn + /` = cursor right
- `Fn + ;` = cursor up
- backtick = `ESC`
- Cardputer `Opt` = MSX `GRAPH`
- Cardputer `Alt` = MSX `CODE`
- Cardputer Shift/Ctrl/Caps map to MSX Shift/Ctrl/Caps

`JOY EXTEND` extras:

- `;` = MSX cursor up
- `.` = MSX cursor down
- `,` = MSX cursor left
- `/` = MSX cursor right
- `1` to `5` = MSX `F1` to `F5`

## Virtual Key Picker

When `JOY EXTEND` or `KEYB/JOY` is active, press `V` to open a small on-screen
key picker. Use left/right to choose a character and `PRIMARY`, `START`, or
Enter to inject it as an MSX keyboard key. `SECONDARY`, `MENU`, or `V` cancels.

The picker includes:

```text
0 1 2 3 4 5 6 7 8 9 ' a b c ... z SPACE ENTER
```

On screen, SPACE is displayed as `_` and ENTER as `E`.

## Display Modes

The firmware can run on:

- internal Cardputer LCD
- external SPI TFT

When the game runs on the external display, the internal LCD remains on and
shows the ROM title and controls. It is not continuously redrawn during external
gameplay, which saves time and avoids fighting the external video path.

When the game runs on the internal LCD, the external TFT remains on and shows a
static MSX1/MSX2 title panel plus the current game name. It is drawn once at
launch or CAS change, then left untouched to avoid spending CPU time on an idle
screen.

View choices are saved and can be changed from `Config Menu`, `Config Keys`, or
with the runtime view shortcut.

## About And Easter Egg

The startup `About` page shows:

```text
Msx ADV Emulators v0.5
MSX is a registered trademark owned by MSX Licensing Corporation
```

There is also a small hidden input tester in the About page. It is intended for
diagnosing how Cardputer keys are seen by the MSX layer in `JOY EXTEND`,
`KEYB/JOY`, `BasicKeyboard`, and `Vaus` modes.

## Differences From fMSX

This project uses fMSX and EMULib lineage where it makes sense, especially
around the Z80/MSX heritage, but v0.5 is not a stock fMSX port.

Major differences:

- Cardputer-specific startup, ROM selector, SD indexing, and NVS settings.
- Dual display workflow for internal LCD and external SPI TFT.
- Flash-partition MSX2 BIOS cache to avoid heap pressure from `MSX2EXT.ROM`.
- Custom MSX boot integration for MSX1/MSX2, sub-ROM, disk, and CAS workflows.
- Local VDP work for Cardputer rendering, external display output, and MSX2
  modes on an ESP32-S3 without PSRAM.
- Runtime menus, save states, CAS macros, control editor, and virtual key
  picker are project-specific.
- Hardware-specific input handling for Cardputer keyboard, `G0`, and optional
  M5Stack I2C joystick.

Known limitations compared with a full desktop MSX emulator:

- YM2413/FM-PAC audio synthesis is not enabled in this build.
- SCC and SCC-I/SCC+ audio synthesis is not included in this build.
- The goal is practical playability on Cardputer hardware, not cycle-perfect
  emulation of every MSX peripheral.
- Official BIOS ROMs are not distributed. You must provide your own legally
  obtained BIOS files if you want the official MSX2 path.

## Build

Recommended release environment:

```text
m5stack-stamps3-max-spiffs-msx2-flash
```

Build:

```powershell
C:\Users\user\.platformio\penv\Scripts\platformio.exe run -e m5stack-stamps3-max-spiffs-msx2-flash
```

The flash layout is defined by:

```text
partitions_a2600_8mb_msx2bios.csv
```

Layout summary:

- app partition: `0x340000`
- SPIFFS/ROM data partition: `0x4A4000`
- MSX2 BIOS cache partition `msx2bios`: `0xC000`

To regenerate the merged v0.5 flashable image:

```powershell
C:\Users\user\.platformio\penv\Scripts\python.exe C:\Users\user\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32s3 merge_bin -o release\MsxADV-Emulators-v0.5-m5stack-stamps3-max-spiffs-msx2-flash-flashable.bin --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0 .pio\build\m5stack-stamps3-max-spiffs-msx2-flash\bootloader.bin 0x8000 .pio\build\m5stack-stamps3-max-spiffs-msx2-flash\partitions.bin 0xe000 C:\Users\user\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin 0x10000 .pio\build\m5stack-stamps3-max-spiffs-msx2-flash\firmware.bin
```

Keep flash mode as `DIO`.

## Credits

Thanks to:

- Marat Fayzullin for fMSX and EMULib.
- the esplay-fMSX project for ESP32-oriented fMSX/Z80 adaptation work.
- /u/geo-tp and the Cardputer Game Station emulator work that helped inspire
  this Cardputer firmware direction.
- M5Stack and the Cardputer community for testing, hardware, and feedback.
