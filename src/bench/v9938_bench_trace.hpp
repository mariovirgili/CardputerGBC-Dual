#pragma once

#ifdef ENABLE_V9938_BENCH
#include "v9938_bench.hpp"

#define V9938_BENCH_SET_CONTEXT(backend, mode, rom_name) \
    do { V9938Bench::setContext((backend), (mode), (rom_name)); } while (0)
#define V9938_BENCH_FRAME_START(rom_name) \
    do { V9938Bench::setContext("msx-vdp", "passive", (rom_name)); V9938Bench::onFrameStart(); } while (0)
#define V9938_BENCH_FRAME_END() \
    do { V9938Bench::onFrameEnd(); } while (0)
#define V9938_BENCH_VRAM_READ(addr, len) \
    do { V9938Bench::onVramRead((addr), (len)); } while (0)
#define V9938_BENCH_VRAM_WRITE(addr, len) \
    do { V9938Bench::onVramWrite((addr), (len)); } while (0)
#define V9938_BENCH_VDP_COMMAND(name, src, dst, len) \
    do { V9938Bench::onVdpCommand((name), (src), (dst), (len)); } while (0)

#else

#define V9938_BENCH_SET_CONTEXT(backend, mode, rom_name) do { } while (0)
#define V9938_BENCH_FRAME_START(rom_name) do { } while (0)
#define V9938_BENCH_FRAME_END() do { } while (0)
#define V9938_BENCH_VRAM_READ(addr, len) do { } while (0)
#define V9938_BENCH_VRAM_WRITE(addr, len) do { } while (0)
#define V9938_BENCH_VDP_COMMAND(name, src, dst, len) do { } while (0)

#endif

