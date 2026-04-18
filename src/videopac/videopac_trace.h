#ifndef VIDEOPAC_TRACE_H
#define VIDEOPAC_TRACE_H

#include <stddef.h>
#include <stdint.h>

#ifndef VIDEOPAC_TRACE_ENABLED
#define VIDEOPAC_TRACE_ENABLED 1
#endif

#if VIDEOPAC_TRACE_ENABLED

uint64_t videopac_trace_now_us();
void videopac_trace_reset(const char* session_name);
void videopac_trace_mark(const char* component, const char* event);
void videopac_trace_printf(const char* component, const char* fmt, ...);
void videopac_trace_frame_sample(const char* stage, uint64_t elapsed_us);
void videopac_trace_frame_report(uint32_t frames,
                                 float fps,
                                 int video_width,
                                 int video_height,
                                 int video_pitch,
                                 size_t audio_frames,
                                 int64_t lateness_us);

class VideopacTraceScope {
public:
    VideopacTraceScope(const char* component, const char* event);
    ~VideopacTraceScope();

    VideopacTraceScope(const VideopacTraceScope&) = delete;
    VideopacTraceScope& operator=(const VideopacTraceScope&) = delete;

private:
    const char* component_;
    const char* event_;
    uint64_t start_us_;
};

#else

static inline uint64_t videopac_trace_now_us() { return 0; }
static inline void videopac_trace_reset(const char*) {}
static inline void videopac_trace_mark(const char*, const char*) {}
static inline void videopac_trace_printf(const char*, const char*, ...) {}
static inline void videopac_trace_frame_sample(const char*, uint64_t) {}
static inline void videopac_trace_frame_report(uint32_t, float, int, int, int, size_t, int64_t) {}

class VideopacTraceScope {
public:
    VideopacTraceScope(const char*, const char*) {}
};

#endif

#endif
