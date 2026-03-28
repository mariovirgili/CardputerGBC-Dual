# Cardputer Game Station Dual Screen

![GameStation 1.0 Dual Screen](images/Externaltitle.png)

Dual-screen firmware branch for the M5Stack Cardputer with an external SPI TFT.

This branch keeps the original emulator pack and adds:

- dual-screen startup and ROM browser screens
- per-core display target selection (`External TFT` or `Internal LCD`)
- per-core control profiles saved on the SD card
- Atari 2600 (`.a26`) and Atari 7800 (`.a78`) launcher integration
- safer ROM loading with size checks before copying to flash
- persistent ROM browser state (last folder and last launched game)
- merged flashable firmware images in the `release/` folder

The project is powered by [Nofrendo](https://github.com/moononournation/arduino-nofrendo), [Snes9x](https://github.com/snes9xgit/snes9x), [Smsplus](https://github.com/ducalex/retro-go/tree/master/retro-core/components/smsplus), [Race](https://github.com/libretro/RACE), [Gwenesis](https://github.com/bzhxx/gwenesis), [Oswan](https://github.com/alekmaul/oswan), [GnuBoy](https://github.com/rofl0r/gnuboy), [Handy](https://github.com/libretro/libretro-handy), [PCE-GO](https://github.com/ducalex/retro-go/tree/master/retro-core/components/pce-go), [Stella](https://stella-emu.github.io/) and [prosystem-libretro](https://github.com/libretro/prosystem-libretro).

![NES emulator screen captures on the M5Stack Cardputer](images/nes_emulator_s.jpg)
![GBC emulator screen captures on the M5Stack Cardputer](images/gbc_emulator_s.jpg)
![SMS emulator screen captures on the M5Stack Cardputer](images/sms_emulator_s.jpg)
![NGP emulator screen captures on the M5Stack Cardputer](images/ngp_emulator_s.jpg)
![Megadrive emulator screen captures on the M5Stack Cardputer](images/megadrive_emulator_s.jpg)

## Supported Systems

| Console | Sound | Video | Save | Speed | Notes |
| --- | --- | --- | --- | --- | --- |
| NES | Yes | Yes | Yes | Full | A few mapper issues remain |
| Game Boy / Game Boy Color | Yes | Yes | Yes | Full | Real DMG/CGB detection on launch |
| Master System | Yes | Yes | Yes | Full | Fully playable |
| Game Gear | Yes | Yes | Yes | Full | Fully playable |
| PC Engine | Yes | Yes | Partial | Full | Very good overall compatibility |
| Lynx | Yes | Yes | Partial | Mostly full | Some heavy titles can slow down |
| Mega Drive / Genesis | Yes | Yes | Partial | Mostly full | Some heavy titles can slow down |
| Neo Geo Pocket / Color | Yes | Yes | Partial | Mostly full | Mono/color support |
| WonderSwan / Color | Yes | Yes | Yes | Partial | Not full speed in all titles |
| Super NES | Partial | Yes | Partial | Partial | Experimental due RAM limits |
| Atari 2600 | Yes | Yes | No | Mostly full | `.a26` only in current branch |
| Atari 7800 | Yes | Yes | No | Partial | `.a78` only, NTSC/PAL supported, some titles are slow |

Supported ROM extensions from SD:

`*.nes *.gb *.gbc *.sms *.gg *.ngc *.ngp *.md *.ws *.wsc *.pce *.lnx *.sfc *.smc *.a26 *.a78`

ROMs must be uncompressed. Do not use `.zip`, `.7z` or `.rar`.
Atari support currently exposes `.a26` and `.a78` only. Generic `.bin` loading is not enabled in this branch.

## Branch Highlights

### Dual-screen workflow

- Internal LCD shows the startup splash and the supported-systems screen.
- External TFT shows a dedicated full-screen title image at boot.
- While browsing ROMs, the external TFT shows a `Select Rom!` panel with supported systems.
- For cores that support both screens, the firmware asks which display to use before launch.
- When `External TFT` is selected, the firmware also asks for color depth:
  - `16-bit 65K colors`
  - `12-bit 4K colors`
- Display target and color depth are saved per core in NVS.
- Atari 2600 and Atari 7800 also expose an internal LCD view mode in the shared config menu:
  - `Pixel Perfect`
  - `Wide`

### Game Boy / Game Boy Color routing

Game Boy handling is now based on the real ROM hardware type, not only the file extension:

- DMG and SGB titles are rendered on the external TFT
- CGB titles are rendered on the internal LCD
- when a CGB game runs on the internal LCD, the external TFT shows the game name and control help

### Atari 2600 and Atari 7800

- Atari 2600 is integrated as a dedicated module under `src/atari2600/` using vendored Stella sources.
- Atari 7800 is integrated as a separate module under `src/atari7800/` using a small local libretro host for `prosystem-libretro`.
- Both Atari modules keep the existing launcher flow:
  - browse ROM on SD
  - copy ROM to flash/XIP
  - dispatch to a dedicated `run_*()` wrapper
- Both support the shared control editor and per-core display target selection.
- Both support internal LCD `Pixel Perfect` and `Wide` modes through the shared config menu.
- Atari 7800 supports both NTSC and PAL timing from the core AV info.

### Per-core control profiles

Controls are no longer fixed globally. Each core has its own `.opt` file on the SD root:

- `NES.opt`
- `SMS.opt`
- `NGP.opt`
- `WS.opt`
- `PCE.opt`
- `GBC.opt`
- `LYNX.opt`
- `GENESIS.opt`
- `SNES.opt`
- `A2600.opt`
- `A7800.opt`

You can edit the current core bindings from the pre-launch control screen with a long press on `GO`.

### ROM browser quality-of-life changes

- The browser remembers the last visited folder in `/.cardputer/last_rom_folder.txt`
- The browser remembers the last launched ROM in NVS
- The cursor repositions itself on the last ROM when you reopen the same folder
- If the remembered ROM or folder no longer exists, the firmware falls back to browsing the SD card instead of getting stuck on `No ROM selected`
- The browser reopens automatically when a selected ROM is too large or cannot be read

### Safer ROM loading

Before copying a ROM to flash, the firmware checks the real file size against the active ROM partition.

If the ROM is too large:

- the game is not started
- an error is shown
- the ROM browser is reopened so you can pick another file

### SD reliability

SD initialization is more robust in this branch:

- the SPI bus is reset before mounting
- CS is forced high before init
- mount retries use several SPI speeds, from 40 MHz down to 1 MHz
- root directory access is verified after mount

## ROM Browser Controls

Inside the ROM selector:

- `E` = up
- `Z` = down
- `A` = jump backward by 4 entries
- `D` = jump forward by 4 entries
- hold `A` or `D` = fast repeat scrolling
- `P` or `Enter` = open folder / select ROM
- `K` = go back to parent folder
- typing letters/numbers = filter the list
- `Del` = remove characters from the filter

On the `RESUME LAST GAME?` prompt:

- `D`, `Right` or `Enter` = yes
- `A`, `Left` or `GO` = no

## In-game and Pre-launch Controls

### Global runtime keys

- `GO` short press during emulation = quit safely and return to the ROM browser
- backtick long press during emulation = quit safely and return to the ROM browser
- `+` / `-` = audio volume
- `[` / `]` = LCD brightness
- `\` = screen mode toggle
- `Fn + Left / Right` = zoom out / zoom in

### Pre-launch screen

Before a game starts, the firmware shows the current bindings for the selected core.

- any normal key starts the game
- long press `GO` opens the control editor for the current core

### Default bindings

The defaults depend on the core, but the base layout is:

- directions: `E`, `S`, `A`, `D`
- 2-button cores: `K`, `L`
- `Start`: `1`
- `Select`: `2`

Additional defaults:

- Genesis adds `J` for button `C`
- SNES uses `I`, `J`, `O`, `P`, `K`, `L`
- WonderSwan exposes both directional groups (`X1..X4`, `Y1..Y4`)

## ROMs and Saves

ROMs can be placed anywhere on the SD card.

Save files are created automatically in per-console folders on the SD card and are linked to the ROM filename.

The quit path waits for pending save activity before rebooting back to the selector, so using `GO` is the safe way to leave a game.

## Flash Layouts

This branch includes multiple partition CSV files. The recommended release environment is:

- `m5stack-stamps3-max-spiffs`

It currently uses:

- [`partitions_a2600_8mb.csv`](partitions_a2600_8mb.csv)

Layout:

- app partition: `0x340000` bytes
- ROM partition (`spiffs`): `0x4B0000` bytes

That gives roughly `4.69 MiB` of ROM storage in the current recommended build, with a larger app partition for the integrated Atari cores.

The older launcher-driven runtime repartitioning flow is disabled in this branch. Partition layout is chosen at build/flash time instead.

## Build and Flash

Recommended build:

```powershell
C:\Users\user\.platformio\penv\Scripts\platformio.exe run --environment m5stack-stamps3-max-spiffs
```

The merged flashable image is stored at:

- [`release/CardputerGBC-Dual-max-spiffs-flashable.bin`](release/CardputerGBC-Dual-max-spiffs-flashable.bin)

Important: when regenerating the merged image manually, keep the bootloader flash mode (`DIO`). Do not force `QIO`.

Example flash command:

```powershell
C:\Users\user\.platformio\penv\Scripts\python.exe -X utf8 C:\Users\user\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32s3 --port COM4 --baud 921600 write_flash 0x0 release\CardputerGBC-Dual-max-spiffs-flashable.bin
```

## M5Stack Joystick

You can also use the M5Stack Joystick v1.1 (U024-C) or Joystick2 (U024-V2). Plug it in before launching a game and it will be detected automatically.

<img src="images/m5stack_joysticks.jpg" alt="A photo of the M5Stack Joysticks" width="800" height="400">

## D-Pad 3D Model

[Cardputer-Accessories repo](https://github.com/AndreiVladescu/Cardputer-Accessories) contains a printable D-Pad model that fits the Cardputer keyboard. Thanks to @AndreiVladescu.

[![A render of the 3D DPAD model](images/cardputer_gamepad_render.jpg)](https://github.com/AndreiVladescu/Cardputer-Accessories)
