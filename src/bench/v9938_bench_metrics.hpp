#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef V9938_BENCH_FRAMES
#define V9938_BENCH_FRAMES 300
#endif

#ifndef V9938_BENCH_MAX_FRAME_SAMPLES
#define V9938_BENCH_MAX_FRAME_SAMPLES 1024
#endif

struct V9938BenchCommandCounters {
    uint64_t hmmm;
    uint64_t lmmm;
    uint64_t hmmc;
    uint64_t lmmc;
    uint64_t line;
    uint64_t fill;
    uint64_t other;
};

struct V9938BenchMetrics {
    uint64_t totalVramReads;
    uint64_t totalVramWrites;
    uint64_t totalVramReadBytes;
    uint64_t totalVramWriteBytes;
    uint64_t totalVdpCommands;
    uint64_t commandBytes;
    uint64_t frames;
    uint64_t totalFrameUs;
    uint32_t frameDurationsUs[V9938_BENCH_MAX_FRAME_SAMPLES];
    size_t frameDurationCount;
    V9938BenchCommandCounters commands;
};

