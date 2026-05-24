# XIP Launch Reference

The common launcher flow is:

1. Select ROM from SD.
2. Detect ROM type from extension.
3. Copy ROM into the flash ROM partition.
4. Memory-map the partition as XIP.
5. Register or update the VFS mapping if needed.
6. Flush input before launch.
7. Launch the selected core from the mapped ROM pointer and mapped size.

Keep core code no-copy when possible. If a core needs a transformed ROM view, prefer lazy conversion or a bounded working buffer over duplicating the whole ROM in internal RAM.

Large ROM handling should return to the selector cleanly. If a ROM cannot fit the current launcher layout, show a clear user-facing error and avoid entering the emulator.
