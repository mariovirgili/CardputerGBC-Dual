#pragma once

#include <stddef.h>
#include <stdint.h>

#include "v9938_bench_metrics.hpp"

class V9938Bench {
public:
    static void init();
    static void start();
    static void stop();
    static bool enabled();

    static void setContext(const char* backend, const char* mode, const char* romName);

    static void onFrameStart();
    static void onFrameEnd();

    static void onVramRead(uint32_t addr, size_t len);
    static void onVramWrite(uint32_t addr, size_t len);
    static void onVdpCommand(const char* name, uint32_t src, uint32_t dst, size_t len);

    static void dumpSummary();
    static void writeCsvRow();
};

uint64_t benchNowNs();

#ifdef V9938_BENCH_SYNTHETIC_TRACE
void v9938BenchRunSyntheticTrace();
#endif
