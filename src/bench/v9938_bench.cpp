#include "v9938_bench.hpp"

#ifdef ENABLE_V9938_BENCH

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_timer.h>

#ifndef V9938_BENCH_SERIAL_OUTPUT
#define V9938_BENCH_SERIAL_OUTPUT 1
#endif

#ifndef V9938_BENCH_VERBOSE
#define V9938_BENCH_VERBOSE 0
#endif

namespace {

struct V9938BenchState {
    bool initialized;
    bool running;
    bool stopped;
    bool frameActive;
    char backend[24];
    char mode[16];
    char romName[96];
    uint64_t startUs;
    uint64_t frameStartUs;
    V9938BenchMetrics metrics;
};

static V9938BenchState s_bench;

uint64_t benchNowUs()
{
    return static_cast<uint64_t>(esp_timer_get_time());
}

void benchCopyField(char* dst, size_t dstSize, const char* src)
{
    if (!dst || dstSize == 0u) {
        return;
    }

    if (!src || src[0] == '\0') {
        std::snprintf(dst, dstSize, "-");
        return;
    }

    size_t i = 0;
    for (; i + 1u < dstSize && src[i] != '\0'; ++i) {
        const char ch = src[i];
        dst[i] = (ch == ',' || ch == '\r' || ch == '\n') ? '_' : ch;
    }
    dst[i] = '\0';
}

double benchSeconds(uint64_t elapsedUs)
{
    return elapsedUs > 0u ? static_cast<double>(elapsedUs) / 1000000.0 : 0.0;
}

double benchMegabytes(uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

uint64_t benchElapsedUs()
{
    if (!s_bench.running && s_bench.startUs == 0u) {
        return 0u;
    }
    const uint64_t nowUs = benchNowUs();
    return nowUs >= s_bench.startUs ? (nowUs - s_bench.startUs) : 0u;
}

double benchRateMbps(uint64_t bytes, uint64_t elapsedUs)
{
    const double seconds = benchSeconds(elapsedUs);
    return seconds > 0.0 ? benchMegabytes(bytes) / seconds : 0.0;
}

double benchPerFrame(uint64_t value)
{
    return s_bench.metrics.frames > 0u
        ? static_cast<double>(value) / static_cast<double>(s_bench.metrics.frames)
        : 0.0;
}

double benchPercent(uint64_t value, uint64_t total)
{
    return total > 0u ? (static_cast<double>(value) * 100.0) / static_cast<double>(total) : 0.0;
}

const char* benchDominantCommand(uint64_t* count)
{
    const V9938BenchCommandCounters& commands = s_bench.metrics.commands;
    const char* name = "NONE";
    uint64_t best = 0u;

    if (commands.hmmm > best) {
        best = commands.hmmm;
        name = "HMMM";
    }
    if (commands.lmmm > best) {
        best = commands.lmmm;
        name = "LMMM";
    }
    if (commands.hmmc > best) {
        best = commands.hmmc;
        name = "HMMC";
    }
    if (commands.lmmc > best) {
        best = commands.lmmc;
        name = "LMMC";
    }
    if (commands.line > best) {
        best = commands.line;
        name = "LINE";
    }
    if (commands.fill > best) {
        best = commands.fill;
        name = "FILL";
    }
    if (commands.other > best) {
        best = commands.other;
        name = "OTHER";
    }

    if (count) {
        *count = best;
    }
    return name;
}

uint32_t benchP99FrameUs()
{
    const size_t count = s_bench.metrics.frameDurationCount;
    if (count == 0u) {
        return 0u;
    }

    // Sort the stored samples in place to avoid a large temporary on loopTask's stack.
    std::sort(s_bench.metrics.frameDurationsUs, s_bench.metrics.frameDurationsUs + count);

    size_t index = static_cast<size_t>((static_cast<uint64_t>(count) * 99u + 99u) / 100u);
    if (index == 0u) {
        index = 1u;
    }
    --index;
    if (index >= count) {
        index = count - 1u;
    }
    return s_bench.metrics.frameDurationsUs[index];
}

void benchCountCommand(const char* name)
{
    if (!name) {
        s_bench.metrics.commands.other++;
        return;
    }

    if (std::strcmp(name, "HMMM") == 0) {
        s_bench.metrics.commands.hmmm++;
    } else if (std::strcmp(name, "LMMM") == 0) {
        s_bench.metrics.commands.lmmm++;
    } else if (std::strcmp(name, "HMMC") == 0) {
        s_bench.metrics.commands.hmmc++;
    } else if (std::strcmp(name, "LMMC") == 0) {
        s_bench.metrics.commands.lmmc++;
    } else if (std::strcmp(name, "LINE") == 0) {
        s_bench.metrics.commands.line++;
    } else if (std::strcmp(name, "HMMV") == 0 || std::strcmp(name, "LMMV") == 0) {
        s_bench.metrics.commands.fill++;
    } else {
        s_bench.metrics.commands.other++;
    }
}

void benchPrintProgress(uint64_t elapsedUs)
{
#if V9938_BENCH_SERIAL_OUTPUT && V9938_BENCH_VERBOSE
    const double seconds = benchSeconds(elapsedUs);
    const double fps = seconds > 0.0 ? static_cast<double>(s_bench.metrics.frames) / seconds : 0.0;
    std::printf("[V9938Bench] frame=%llu fps=%.1f read=%.2fMB/s write=%.2fMB/s cmd=HMMM:%llu LMMM:%llu HMMC:%llu LMMC:%llu LINE:%llu FILL:%llu\n",
                static_cast<unsigned long long>(s_bench.metrics.frames),
                fps,
                benchRateMbps(s_bench.metrics.totalVramReadBytes, elapsedUs),
                benchRateMbps(s_bench.metrics.totalVramWriteBytes, elapsedUs),
                static_cast<unsigned long long>(s_bench.metrics.commands.hmmm),
                static_cast<unsigned long long>(s_bench.metrics.commands.lmmm),
                static_cast<unsigned long long>(s_bench.metrics.commands.hmmc),
                static_cast<unsigned long long>(s_bench.metrics.commands.lmmc),
                static_cast<unsigned long long>(s_bench.metrics.commands.line),
                static_cast<unsigned long long>(s_bench.metrics.commands.fill));
#else
    (void)elapsedUs;
#endif
}

} // namespace

uint64_t benchNowNs()
{
    return benchNowUs() * 1000ull;
}

void V9938Bench::init()
{
    if (s_bench.initialized) {
        return;
    }

    std::memset(&s_bench, 0, sizeof(s_bench));
    benchCopyField(s_bench.backend, sizeof(s_bench.backend), "msx-vdp");
    benchCopyField(s_bench.mode, sizeof(s_bench.mode), "passive");
    benchCopyField(s_bench.romName, sizeof(s_bench.romName), "-");
    s_bench.initialized = true;
}

void V9938Bench::start()
{
    init();
    if (s_bench.running || s_bench.stopped) {
        return;
    }

    s_bench.startUs = benchNowUs();
    s_bench.running = true;
#if V9938_BENCH_SERIAL_OUTPUT
    std::printf("[V9938Bench] start backend=%s mode=%s rom=%s frames=%u\n",
                s_bench.backend,
                s_bench.mode,
                s_bench.romName,
                static_cast<unsigned>(V9938_BENCH_FRAMES));
#endif
}

void V9938Bench::stop()
{
    if (!s_bench.initialized || s_bench.stopped) {
        return;
    }

    s_bench.running = false;
    s_bench.frameActive = false;
    s_bench.stopped = true;
}

bool V9938Bench::enabled()
{
    return s_bench.initialized && s_bench.running && !s_bench.stopped;
}

void V9938Bench::setContext(const char* backend, const char* mode, const char* romName)
{
    init();
    if (s_bench.running || s_bench.stopped) {
        return;
    }

    benchCopyField(s_bench.backend, sizeof(s_bench.backend), backend);
    benchCopyField(s_bench.mode, sizeof(s_bench.mode), mode);
    benchCopyField(s_bench.romName, sizeof(s_bench.romName), romName);
}

void V9938Bench::onFrameStart()
{
    init();
    if (s_bench.stopped) {
        return;
    }
    if (!s_bench.running) {
        start();
    }

    s_bench.frameStartUs = benchNowUs();
    s_bench.frameActive = true;
}

void V9938Bench::onFrameEnd()
{
    if (!s_bench.running || !s_bench.frameActive) {
        return;
    }

    const uint64_t nowUs = benchNowUs();
    const uint32_t frameUs = nowUs >= s_bench.frameStartUs
        ? static_cast<uint32_t>(nowUs - s_bench.frameStartUs)
        : 0u;

    s_bench.metrics.frames++;
    s_bench.metrics.totalFrameUs += frameUs;
    if (s_bench.metrics.frameDurationCount < V9938_BENCH_MAX_FRAME_SAMPLES) {
        s_bench.metrics.frameDurationsUs[s_bench.metrics.frameDurationCount++] = frameUs;
    }
    s_bench.frameActive = false;

    const uint64_t elapsedUs = benchElapsedUs();
    if ((s_bench.metrics.frames % 60u) == 0u) {
        benchPrintProgress(elapsedUs);
    }

    if (s_bench.metrics.frames >= static_cast<uint64_t>(V9938_BENCH_FRAMES)) {
        dumpSummary();
        writeCsvRow();
        stop();
    }
}

void V9938Bench::onVramRead(uint32_t, size_t len)
{
    if (!s_bench.running || s_bench.stopped) {
        return;
    }

    s_bench.metrics.totalVramReads++;
    s_bench.metrics.totalVramReadBytes += len;
}

void V9938Bench::onVramWrite(uint32_t, size_t len)
{
    if (!s_bench.running || s_bench.stopped) {
        return;
    }

    s_bench.metrics.totalVramWrites++;
    s_bench.metrics.totalVramWriteBytes += len;
}

void V9938Bench::onVdpCommand(const char* name, uint32_t, uint32_t, size_t len)
{
    if (!s_bench.running || s_bench.stopped) {
        return;
    }

    s_bench.metrics.totalVdpCommands++;
    s_bench.metrics.commandBytes += len;
    benchCountCommand(name);
}

void V9938Bench::dumpSummary()
{
#if V9938_BENCH_SERIAL_OUTPUT
    if (!s_bench.initialized) {
        return;
    }

    const uint64_t elapsedUs = benchElapsedUs();
    const uint64_t wallMs = elapsedUs / 1000u;
    const double seconds = benchSeconds(elapsedUs);
    const double fps = seconds > 0.0 ? static_cast<double>(s_bench.metrics.frames) / seconds : 0.0;
    const double avgFrameMs = s_bench.metrics.frames > 0u
        ? static_cast<double>(s_bench.metrics.totalFrameUs) / static_cast<double>(s_bench.metrics.frames) / 1000.0
        : 0.0;
    const double p99FrameMs = static_cast<double>(benchP99FrameUs()) / 1000.0;
    const uint64_t totalVramBytes = s_bench.metrics.totalVramReadBytes + s_bench.metrics.totalVramWriteBytes;
    uint64_t dominantCommandCount = 0u;
    const char* dominantCommand = benchDominantCommand(&dominantCommandCount);

    std::printf("[V9938Bench] summary backend=%s mode=%s rom=%s frames=%llu wall_ms=%llu fps=%.2f avg_frame_ms=%.3f p99_frame_ms=%.3f\n",
                s_bench.backend,
                s_bench.mode,
                s_bench.romName,
                static_cast<unsigned long long>(s_bench.metrics.frames),
                static_cast<unsigned long long>(wallMs),
                fps,
                avgFrameMs,
                p99FrameMs);
    std::printf("[V9938Bench] vram reads=%llu writes=%llu read_bytes=%llu write_bytes=%llu read_MBps=%.3f write_MBps=%.3f total_MBps=%.3f\n",
                static_cast<unsigned long long>(s_bench.metrics.totalVramReads),
                static_cast<unsigned long long>(s_bench.metrics.totalVramWrites),
                static_cast<unsigned long long>(s_bench.metrics.totalVramReadBytes),
                static_cast<unsigned long long>(s_bench.metrics.totalVramWriteBytes),
                benchRateMbps(s_bench.metrics.totalVramReadBytes, elapsedUs),
                benchRateMbps(s_bench.metrics.totalVramWriteBytes, elapsedUs),
                benchRateMbps(s_bench.metrics.totalVramReadBytes + s_bench.metrics.totalVramWriteBytes, elapsedUs));
    std::printf("[V9938Bench] commands total=%llu bytes=%llu HMMM=%llu LMMM=%llu HMMC=%llu LMMC=%llu LINE=%llu FILL=%llu OTHER=%llu\n",
                static_cast<unsigned long long>(s_bench.metrics.totalVdpCommands),
                static_cast<unsigned long long>(s_bench.metrics.commandBytes),
                static_cast<unsigned long long>(s_bench.metrics.commands.hmmm),
                static_cast<unsigned long long>(s_bench.metrics.commands.lmmm),
                static_cast<unsigned long long>(s_bench.metrics.commands.hmmc),
                static_cast<unsigned long long>(s_bench.metrics.commands.lmmc),
                static_cast<unsigned long long>(s_bench.metrics.commands.line),
                static_cast<unsigned long long>(s_bench.metrics.commands.fill),
                static_cast<unsigned long long>(s_bench.metrics.commands.other));
    std::printf("[V9938Bench][REPORT] begin\n");
    std::printf("[V9938Bench][REPORT] test rom=%s backend=%s mode=%s frames=%llu wall_ms=%llu\n",
                s_bench.romName,
                s_bench.backend,
                s_bench.mode,
                static_cast<unsigned long long>(s_bench.metrics.frames),
                static_cast<unsigned long long>(wallMs));
    std::printf("[V9938Bench][REPORT] speed fps=%.2f avg_frame_ms=%.3f p99_frame_ms=%.3f realtime_vs_60fps=%.1f%%\n",
                fps,
                avgFrameMs,
                p99FrameMs,
                (fps * 100.0) / 60.0);
    std::printf("[V9938Bench][REPORT] vram total_MBps=%.3f read_MBps=%.3f write_MBps=%.3f total_bytes_per_frame=%.1f reads_per_frame=%.1f writes_per_frame=%.1f\n",
                benchRateMbps(totalVramBytes, elapsedUs),
                benchRateMbps(s_bench.metrics.totalVramReadBytes, elapsedUs),
                benchRateMbps(s_bench.metrics.totalVramWriteBytes, elapsedUs),
                benchPerFrame(totalVramBytes),
                benchPerFrame(s_bench.metrics.totalVramReads),
                benchPerFrame(s_bench.metrics.totalVramWrites));
    std::printf("[V9938Bench][REPORT] commands total=%llu per_frame=%.2f bytes_per_command=%.1f dominant=%s dominant_pct=%.1f%%\n",
                static_cast<unsigned long long>(s_bench.metrics.totalVdpCommands),
                benchPerFrame(s_bench.metrics.totalVdpCommands),
                s_bench.metrics.totalVdpCommands > 0u
                    ? static_cast<double>(s_bench.metrics.commandBytes) / static_cast<double>(s_bench.metrics.totalVdpCommands)
                    : 0.0,
                dominantCommand,
                benchPercent(dominantCommandCount, s_bench.metrics.totalVdpCommands));
    std::printf("[V9938Bench][REPORT] command_mix HMMM=%llu(%.1f%%) LMMM=%llu(%.1f%%) HMMC=%llu(%.1f%%) LMMC=%llu(%.1f%%) LINE=%llu(%.1f%%) FILL=%llu(%.1f%%) OTHER=%llu(%.1f%%)\n",
                static_cast<unsigned long long>(s_bench.metrics.commands.hmmm),
                benchPercent(s_bench.metrics.commands.hmmm, s_bench.metrics.totalVdpCommands),
                static_cast<unsigned long long>(s_bench.metrics.commands.lmmm),
                benchPercent(s_bench.metrics.commands.lmmm, s_bench.metrics.totalVdpCommands),
                static_cast<unsigned long long>(s_bench.metrics.commands.hmmc),
                benchPercent(s_bench.metrics.commands.hmmc, s_bench.metrics.totalVdpCommands),
                static_cast<unsigned long long>(s_bench.metrics.commands.lmmc),
                benchPercent(s_bench.metrics.commands.lmmc, s_bench.metrics.totalVdpCommands),
                static_cast<unsigned long long>(s_bench.metrics.commands.line),
                benchPercent(s_bench.metrics.commands.line, s_bench.metrics.totalVdpCommands),
                static_cast<unsigned long long>(s_bench.metrics.commands.fill),
                benchPercent(s_bench.metrics.commands.fill, s_bench.metrics.totalVdpCommands),
                static_cast<unsigned long long>(s_bench.metrics.commands.other),
                benchPercent(s_bench.metrics.commands.other, s_bench.metrics.totalVdpCommands));
    std::printf("[V9938Bench][REPORT] compare_hint=same_rom_same_bios_same_video_settings_lower_avg_ms_and_p99_ms_are_better\n");
    std::printf("[V9938Bench][REPORT] end\n");
#endif
}

void V9938Bench::writeCsvRow()
{
#if V9938_BENCH_SERIAL_OUTPUT
    if (!s_bench.initialized) {
        return;
    }

    const uint64_t elapsedUs = benchElapsedUs();
    const uint64_t wallMs = elapsedUs / 1000u;
    const double seconds = benchSeconds(elapsedUs);
    const double fps = seconds > 0.0 ? static_cast<double>(s_bench.metrics.frames) / seconds : 0.0;
    const double avgFrameMs = s_bench.metrics.frames > 0u
        ? static_cast<double>(s_bench.metrics.totalFrameUs) / static_cast<double>(s_bench.metrics.frames) / 1000.0
        : 0.0;
    const double p99FrameMs = static_cast<double>(benchP99FrameUs()) / 1000.0;

    std::printf("backend,mode,rom_name,frames,wall_time_ms,fps,avg_frame_time_ms,p99_frame_time_ms,total_vram_reads,total_vram_writes,total_vram_read_bytes,total_vram_write_bytes,total_vdp_commands,command_bytes,vram_read_MBps,vram_write_MBps,total_vram_MBps\n");
    std::printf("%s,%s,%s,%llu,%llu,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%.3f,%.3f\n",
                s_bench.backend,
                s_bench.mode,
                s_bench.romName,
                static_cast<unsigned long long>(s_bench.metrics.frames),
                static_cast<unsigned long long>(wallMs),
                fps,
                avgFrameMs,
                p99FrameMs,
                static_cast<unsigned long long>(s_bench.metrics.totalVramReads),
                static_cast<unsigned long long>(s_bench.metrics.totalVramWrites),
                static_cast<unsigned long long>(s_bench.metrics.totalVramReadBytes),
                static_cast<unsigned long long>(s_bench.metrics.totalVramWriteBytes),
                static_cast<unsigned long long>(s_bench.metrics.totalVdpCommands),
                static_cast<unsigned long long>(s_bench.metrics.commandBytes),
                benchRateMbps(s_bench.metrics.totalVramReadBytes, elapsedUs),
                benchRateMbps(s_bench.metrics.totalVramWriteBytes, elapsedUs),
                benchRateMbps(s_bench.metrics.totalVramReadBytes + s_bench.metrics.totalVramWriteBytes, elapsedUs));
#endif
}

#ifdef V9938_BENCH_SYNTHETIC_TRACE
void v9938BenchRunSyntheticTrace()
{
#if V9938_BENCH_SERIAL_OUTPUT
    std::printf("[V9938Bench] synthetic trace skeleton is compiled but not executed automatically\n");
#endif
}
#endif

#else

uint64_t benchNowNs()
{
    return 0u;
}

void V9938Bench::init() {}
void V9938Bench::start() {}
void V9938Bench::stop() {}
bool V9938Bench::enabled() { return false; }
void V9938Bench::setContext(const char*, const char*, const char*) {}
void V9938Bench::onFrameStart() {}
void V9938Bench::onFrameEnd() {}
void V9938Bench::onVramRead(uint32_t, size_t) {}
void V9938Bench::onVramWrite(uint32_t, size_t) {}
void V9938Bench::onVdpCommand(const char*, uint32_t, uint32_t, size_t) {}
void V9938Bench::dumpSummary() {}
void V9938Bench::writeCsvRow() {}

#endif
