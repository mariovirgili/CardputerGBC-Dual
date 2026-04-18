#include "videopac_trace.h"

#if VIDEOPAC_TRACE_ENABLED

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

struct FrameStageStat {
    const char* name;
    uint64_t total_us;
    uint64_t max_us;
    uint32_t count;
};

constexpr size_t kMaxFrameStageStats = 12;

uint64_t s_session_start_us = 0;
uint64_t s_last_mark_us = 0;
FrameStageStat s_stage_stats[kMaxFrameStageStats] = {};

uint32_t free_heap()
{
    return esp_get_free_heap_size();
}

uint32_t largest_free_block()
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

void reset_stage_stats()
{
    std::memset(s_stage_stats, 0, sizeof(s_stage_stats));
}

FrameStageStat* find_stage_stat(const char* name)
{
    if (!name || !name[0]) {
        return nullptr;
    }

    FrameStageStat* empty = nullptr;
    for (FrameStageStat& stat : s_stage_stats) {
        if (stat.name && std::strcmp(stat.name, name) == 0) {
            return &stat;
        }
        if (!stat.name && !empty) {
            empty = &stat;
        }
    }

    if (empty) {
        empty->name = name;
        return empty;
    }
    return nullptr;
}

void print_prefix(const char* component)
{
    const uint64_t now = videopac_trace_now_us();
    if (s_session_start_us == 0) {
        s_session_start_us = now;
        s_last_mark_us = now;
    }

    const uint64_t since_start = now - s_session_start_us;
    const uint64_t since_last = now - s_last_mark_us;
    s_last_mark_us = now;

    std::printf("[VPTRACE][%8llu ms][+%7llu us][heap=%u largest=%u][%s] ",
                static_cast<unsigned long long>(since_start / 1000),
                static_cast<unsigned long long>(since_last),
                static_cast<unsigned>(free_heap()),
                static_cast<unsigned>(largest_free_block()),
                component ? component : "core");
}

} // namespace

uint64_t videopac_trace_now_us()
{
    return static_cast<uint64_t>(esp_timer_get_time());
}

void videopac_trace_reset(const char* session_name)
{
    s_session_start_us = videopac_trace_now_us();
    s_last_mark_us = s_session_start_us;
    reset_stage_stats();
    std::printf("\n[VPTRACE][       0 ms][+      0 us][heap=%u largest=%u][session] start name=%s\n",
                static_cast<unsigned>(free_heap()),
                static_cast<unsigned>(largest_free_block()),
                session_name ? session_name : "(unnamed)");
}

void videopac_trace_mark(const char* component, const char* event)
{
    print_prefix(component);
    std::printf("%s\n", event ? event : "mark");
}

void videopac_trace_printf(const char* component, const char* fmt, ...)
{
    print_prefix(component);

    va_list ap;
    va_start(ap, fmt);
    if (fmt) {
        std::vprintf(fmt, ap);
    }
    va_end(ap);
    std::printf("\n");
}

void videopac_trace_frame_sample(const char* stage, uint64_t elapsed_us)
{
    FrameStageStat* stat = find_stage_stat(stage);
    if (!stat) {
        return;
    }

    stat->total_us += elapsed_us;
    if (elapsed_us > stat->max_us) {
        stat->max_us = elapsed_us;
    }
    stat->count++;
}

void videopac_trace_frame_report(uint32_t frames,
                                 float fps,
                                 int video_width,
                                 int video_height,
                                 int video_pitch,
                                 size_t audio_frames,
                                 int64_t lateness_us)
{
    print_prefix("perf");
    std::printf("frames=%u fps=%.1f video=%dx%d pitch=%d audio_frames=%u lateness_us=%lld\n",
                static_cast<unsigned>(frames),
                static_cast<double>(fps),
                video_width,
                video_height,
                video_pitch,
                static_cast<unsigned>(audio_frames),
                static_cast<long long>(lateness_us));

    for (const FrameStageStat& stat : s_stage_stats) {
        if (!stat.name || stat.count == 0) {
            continue;
        }
        const uint64_t avg = stat.total_us / stat.count;
        std::printf("[VPTRACE][stage][%-10s] avg=%llu us max=%llu us samples=%u\n",
                    stat.name,
                    static_cast<unsigned long long>(avg),
                    static_cast<unsigned long long>(stat.max_us),
                    static_cast<unsigned>(stat.count));
    }

    reset_stage_stats();
}

VideopacTraceScope::VideopacTraceScope(const char* component, const char* event)
    : component_(component),
      event_(event),
      start_us_(videopac_trace_now_us())
{
    videopac_trace_printf(component_, "%s begin", event_ ? event_ : "scope");
}

VideopacTraceScope::~VideopacTraceScope()
{
    const uint64_t elapsed = videopac_trace_now_us() - start_us_;
    videopac_trace_printf(component_, "%s end elapsed_us=%llu",
                          event_ ? event_ : "scope",
                          static_cast<unsigned long long>(elapsed));
}

#endif
