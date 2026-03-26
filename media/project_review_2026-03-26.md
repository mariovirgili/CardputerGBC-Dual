# Project Review - Dual Screen Branch

Date: 2026-03-26
Branch reviewed: `feature/dual-screen-adv`
Scope: architectural review and weak-point analysis only. No source code was changed for this report.

## Findings

### 1. The current GBC routing does not actually enforce the "DMG on external / CGB on internal" rule
Severity: High

What I see:
- `src/main.cpp:353-404` always asks the user to choose a display target for GB/GBC because `ROM_TYPE_GB` is treated like the other dual-display cores.
- `src/gbc/run_gbc.cpp:98-105` reads the real hardware type with `gnuboy_get_hwtype()`, but then it still uses `g_emu_display_target` for the actual routing.

Why this is weak:
- The code and the branch behavior are currently misaligned.
- The firmware can still launch a CGB title on the external TFT or a DMG title on the internal LCD, depending on the previous selector choice.
- This also means the README and the mental model of the branch are stronger than the actual implementation.

What I would change:
- Move the final routing decision into `run_gbc.cpp` and make the hardware type authoritative.
- Keep the selector only if you want an explicit "override" feature, but then document it as an override instead of "real hardware routing".

### 2. SD file handle management is fragile and leaks handles on negative paths
Severity: High

What I see:
- `src/cardputer/SdService.cpp:49-55` in `isFile()` closes the handle only on the `true` branch.
- `src/cardputer/SdService.cpp:58-64` in `isDirectory()` does the same.

Why this is weak:
- On embedded targets, small handle leaks accumulate quickly.
- A ROM browser repeatedly calling these helpers can degrade into random filesystem failures that are hard to reproduce.

What I would change:
- Always close the `File` object when it was opened, regardless of the return value.

### 3. The ROM browser remounts the SD card instead of reusing the mounted session
Severity: High

What I see:
- `src/main.cpp:236-241` already mounts the SD and blocks until it succeeds.
- `src/select_rom.h:149-152` mounts again inside `getRomPath()`.

Why this is weak:
- Reinitializing SPI and remounting the card every time the browser opens is unnecessary work.
- It increases latency and makes the ROM selector more sensitive to timing/card quality issues.
- It is also consistent with the kind of "SD card not found even if present" symptoms you already saw.

What I would change:
- Mount once in startup, keep a clear mounted/unmounted ownership model, and only remount after a real failure or explicit recovery path.

### 4. Too many recoverable failures still end in hard dead-ends
Severity: Medium

What I see:
- `src/main.cpp:270-275`, `src/main.cpp:323-328`, `src/main.cpp:335-339`, `src/main.cpp:520-522`
- `src/gbc/run_gbc.cpp:53-66`
- `src/genesis/run_genesis.cpp:52-56`
- `src/sms/display.cpp:125-127`

Why this is weak:
- The project has made the ROM flow much friendlier, but several failures still end in `while (1)` loops or `abort()`.
- On real hardware this feels like a freeze, not a controlled failure.
- It also makes field diagnostics harder because the user often needs a hard reset instead of being sent back to the selector with an error message.

What I would change:
- Separate fatal hardware corruption from ordinary runtime failures.
- For normal failures, show an error and return to the ROM browser.
- Reserve `abort()` for truly unrecoverable corruption.

### 5. Flash/release generation is still too brittle
Severity: Medium

What I see:
- `platformio.ini:24` still declares `board_build.flash_mode = qio`.
- The known-good merged release image must preserve `DIO`, and you already hit boot loops when the final merged binary ended up with the wrong flash mode.

Why this is weak:
- A future rebuild can silently regress if the merge step is done differently.
- The release process depends too much on operator memory instead of being encoded in the project.

What I would change:
- Make the release generation reproducible from the project itself.
- Either align the environment so the generated images are unambiguous, or script the merge step so the output always keeps the correct flash mode.

### 6. Input handling is tightly coupled to delays and direct polling, which makes it brittle
Severity: Medium

What I see:
- `src/cardputer/CardputerInput.cpp:13-38` mixes click, long-press suppression and UI semantics in one low-level handler.
- `src/cardputer/CardputerInput.cpp:70`, `src/cardputer/CardputerInput.cpp:91-99`
- `src/share/input.cpp:52-77`

Why this is weak:
- There are multiple small delays in the input path.
- Input semantics are partially menu-specific and partially global.
- This makes accidental double-actions, missed keys and "ghost" transitions more likely, especially when mixing `GO`, resume prompts, pre-launch screens and runtime hotkeys.

What I would change:
- Separate low-level input sampling from higher-level actions.
- Normalize events into `pressed`, `clicked`, `long_pressed`, `repeat`.
- Let UI layers map events to behaviors, instead of hardcoding that logic across several files.

### 7. The project relies on many dynamic allocations without a shared memory budget
Severity: Medium

What I see:
- `src/gbc/gbc_display.cpp:271-287`, `src/gbc/gbc_display.cpp:327-368`
- `src/nes/nes_display.cpp:230-245`
- `src/sms/display.cpp:120-127`, `src/sms/display.cpp:203-221`
- Similar patterns appear in other cores too.

Why this is weak:
- This firmware is already working close to platform limits.
- Multiple renderers allocate caches or line buffers on demand, sometimes with fallback behavior and sometimes with a hard stop.
- Without a central memory policy, one new feature can destabilize an apparently unrelated core.

What I would change:
- Define a memory budget per core and per display mode.
- Decide which caches are mandatory and which are optional.
- Log allocation failures in a consistent way and degrade gracefully where possible.

### 8. Too much application logic lives in implementation-heavy headers
Severity: Medium

What I see:
- `src/select_rom.h:35-229`
- `src/last_game.h:12-195`

Why this is weak:
- These files are not simple declarations; they contain real app logic, storage logic and flow control.
- This makes compile dependencies wider and the architecture harder to reason about.
- It also increases the chance of accidental duplication or inconsistent behavior as the branch grows.

What I would change:
- Move ROM selection and persistence logic into `.cpp` units with small headers.
- Keep headers limited to declarations and simple inline helpers only.

### 9. The ROM selector UI and its support list are slightly inconsistent
Severity: Low

What I see:
- `src/select_rom.h:137` shows a supported extension list that omits `.ngp` and `.smc`.
- `src/select_rom.h:35-47` has duplicate checks for `.gb` and `.gbc`.
- `src/select_rom.h:93-105` does correctly support `.ngp` and `.smc`.

Why this is weak:
- The selector works, but the UI and the implementation are not perfectly aligned.
- This tends to produce subtle "is this supported or not?" confusion over time.

What I would change:
- Keep one single canonical extension table and reuse it for:
  - validation
  - ROM type detection
  - supported list rendering
  - README documentation

## Broader Architectural Weaknesses

### 1. `main.cpp` is carrying too much orchestration
`src/main.cpp:214-524` currently owns:
- startup flow
- SD bootstrap
- last-game recovery
- ROM browser fallback
- ROM copy to flash
- XIP setup
- per-core display selection
- per-core control editor entry
- emulator dispatch

This works, but it is the main concentration point of branch complexity. It is the first file I would split if the project continues to grow.

### 2. The dual-screen logic is repeated core by core
The pattern "game on one screen, info/help on the other" appears in several renderers and launchers. The feature is good, but the implementation is duplicated. Over time that usually creates behavior drift between cores.

### 3. Release engineering is still manual
The branch is already more complex than a simple PlatformIO one-click firmware. At this point, release generation should be encoded as a reproducible project task, not a remembered sequence.

## What I Would Prioritize First

1. Fix the GBC routing so the real hardware type truly decides the target screen.
2. Fix SD handle lifecycle and stop remounting the card inside the browser.
3. Remove the remaining dead-end loops for common user-facing failures.
4. Make flashable release generation deterministic and self-documenting.
5. Split startup/browser/persistence logic out of `main.cpp` and out of heavy headers.

## Positive Notes

- The branch clearly improved usability compared to a plain emulator launcher flow.
- The ROM browser persistence work is directionally good and user-focused.
- The dual-screen UX is ambitious and already gives the firmware a distinct identity.
- The project now has enough structure to benefit a lot from one cleanup pass focused on ownership, failure handling and release reproducibility.
