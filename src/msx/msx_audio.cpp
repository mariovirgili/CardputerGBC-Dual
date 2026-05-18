#pragma GCC optimize ("Os")

#include "msx_host_internal.h"

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "share/emu_log_cpp.h"

#ifdef EMU_LOGS_ENABLED
namespace {

struct MsxPerfStats {
    uint32_t lastLogMs = 0;
    uint32_t frames = 0;
    uint32_t renderedFrames = 0;
    uint32_t audioRate = 0;
    uint32_t audioRenderCalls = 0;
    uint32_t audioWriteCalls = 0;
    uint32_t audioSilentRenders = 0;
    uint32_t audioBusyWrites = 0;
    uint32_t audioRenderRequested = 0;
    uint32_t audioRenderPlayed = 0;
    uint32_t audioWriteRequested = 0;
    uint32_t audioWriteWritten = 0;
    uint32_t audioWriteDropped = 0;
    uint32_t maxQueued = 0;
    uint32_t maxFreeSamples = 0;
    uint32_t maxPendingUsec = 0;
};

static MsxPerfStats s_msxPerf;

static void msx_perf_reset_window(uint32_t lastLogMs, uint32_t audioRate)
{
    std::memset(&s_msxPerf, 0, sizeof(s_msxPerf));
    s_msxPerf.lastLogMs = lastLogMs;
    s_msxPerf.audioRate = audioRate;
}

} // namespace

extern "C" void msx_host_perf_reset(void)
{
    msx_perf_reset_window((uint32_t)millis(), 0);
}

extern "C" void msx_host_perf_note_frame(int renderedFrame, int palVideo, unsigned int uPeriod)
{
    uint32_t now = (uint32_t)millis();
    if (s_msxPerf.lastLogMs == 0) {
        s_msxPerf.lastLogMs = now;
    }

    s_msxPerf.frames++;
    if (renderedFrame) {
        s_msxPerf.renderedFrames++;
    }

    uint32_t elapsed = now - s_msxPerf.lastLogMs;
    if (elapsed < 2000) {
        return;
    }

    const float fps = (s_msxPerf.frames * 1000.0f) / (float)elapsed;
    const float renderedFps = (s_msxPerf.renderedFrames * 1000.0f) / (float)elapsed;
    const uint32_t renderDrop = (s_msxPerf.audioRenderRequested > s_msxPerf.audioRenderPlayed)
        ? (s_msxPerf.audioRenderRequested - s_msxPerf.audioRenderPlayed)
        : 0;

    EMU_LOG("[MSX] FPS %.2f | RENDER %.2f | HEAP %u | AUD rate=%u render=%u req/play/drop=%u/%u/%u write=%u req/wr/drop=%u/%u/%u busy=%u silent=%u qMax=%u freeMax=%u pendingMax=%u uPeriod=%u video=%s\n",
            fps,
            renderedFps,
            esp_get_free_heap_size(),
            s_msxPerf.audioRate,
            s_msxPerf.audioRenderCalls,
            s_msxPerf.audioRenderRequested,
            s_msxPerf.audioRenderPlayed,
            renderDrop,
            s_msxPerf.audioWriteCalls,
            s_msxPerf.audioWriteRequested,
            s_msxPerf.audioWriteWritten,
            s_msxPerf.audioWriteDropped,
            s_msxPerf.audioBusyWrites,
            s_msxPerf.audioSilentRenders,
            s_msxPerf.maxQueued,
            s_msxPerf.maxFreeSamples,
            s_msxPerf.maxPendingUsec,
            uPeriod,
            palVideo ? "PAL" : "NTSC");

    const uint32_t audioRate = s_msxPerf.audioRate;
    msx_perf_reset_window(now, audioRate);
}

extern "C" void msx_host_audio_note_init(unsigned int rate)
{
    s_msxPerf.audioRate = rate;
}

extern "C" void msx_host_audio_note_render(unsigned int requestedSamples, unsigned int playedSamples, unsigned int freeSamplesBefore)
{
    s_msxPerf.audioRenderCalls++;
    s_msxPerf.audioRenderRequested += requestedSamples;
    s_msxPerf.audioRenderPlayed += playedSamples;
    if (playedSamples == 0) {
        s_msxPerf.audioSilentRenders++;
    }
    if (freeSamplesBefore > s_msxPerf.maxFreeSamples) {
        s_msxPerf.maxFreeSamples = freeSamplesBefore;
    }
    const uint32_t pending = (uint32_t)msx::g_host.pendingUsec;
    if (pending > s_msxPerf.maxPendingUsec) {
        s_msxPerf.maxPendingUsec = pending;
    }
}

extern "C" void msx_host_audio_note_write(unsigned int requestedSamples, unsigned int writtenSamples, unsigned int queuedBefore)
{
    s_msxPerf.audioWriteCalls++;
    s_msxPerf.audioWriteRequested += requestedSamples;
    s_msxPerf.audioWriteWritten += writtenSamples;
    if (requestedSamples > writtenSamples) {
        s_msxPerf.audioWriteDropped += requestedSamples - writtenSamples;
    }
    if (writtenSamples == 0 && requestedSamples > 0) {
        s_msxPerf.audioBusyWrites++;
    }
    if (queuedBefore > s_msxPerf.maxQueued) {
        s_msxPerf.maxQueued = queuedBefore;
    }
}
#endif

extern "C" unsigned int InitAudio(unsigned int Rate, unsigned int)
{
    using namespace msx;

    if (!g_host.audioBuf) {
        g_host.audioBuf = (sample**)std::calloc(kAudioBufCount, sizeof(sample*));
    }
    if (!g_host.audioBuf) return 0;

    auto cfg = M5Cardputer.Speaker.config();
    cfg.sample_rate      = Rate;
    cfg.stereo           = false;
    cfg.dma_buf_len      = 512;
    cfg.dma_buf_count    = 8;
    cfg.task_priority    = 4;
    cfg.task_pinned_core = 0;
    M5Cardputer.Speaker.config(cfg);

    if (!M5Cardputer.Speaker.isRunning()) {
        M5Cardputer.Speaker.begin();
    }

    M5Cardputer.Speaker.setVolume(60);
    M5Cardputer.Speaker.stop(kAudioChannel);

    for (int i = 0; i < kAudioBufCount; ++i) {
        if (!g_host.audioBuf[i]) {
            g_host.audioBuf[i] = (sample*)heap_caps_malloc(
                kAudioChunk * sizeof(sample),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT
            );
        }
    }

    g_host.audioBufIndex = 0;
    g_host.pendingUsec = 0;
#ifdef EMU_LOGS_ENABLED
    msx_host_audio_note_init(Rate);
#endif
    return Rate;
}

extern "C" void TrashAudio(void)
{
    M5Cardputer.Speaker.stop(msx::kAudioChannel);

    if (msx::g_host.audioBuf) {
        for (int i = 0; i < msx::kAudioBufCount; ++i) {
            std::free(msx::g_host.audioBuf[i]);
            msx::g_host.audioBuf[i] = nullptr;
        }
        std::free(msx::g_host.audioBuf);
        msx::g_host.audioBuf = nullptr;
    }
}

extern "C" unsigned int GetFreeAudio(void)
{
    const size_t queued = M5Cardputer.Speaker.isPlaying(msx::kAudioChannel);
    if (queued == 0) return msx::kAudioChunk * 2;
    if (queued == 1) return msx::kAudioChunk;
    return 0;
}

extern "C" unsigned int WriteAudio(sample* data, unsigned int length)
{
    using namespace msx;

    if (!data || !length || length > (unsigned)kAudioChunk || !g_host.audioBuf) return 0;

    const unsigned int queuedBefore = (unsigned int)M5Cardputer.Speaker.isPlaying(kAudioChannel);
    if (queuedBefore > 1) {
#ifdef EMU_LOGS_ENABLED
        msx_host_audio_note_write(length, 0, queuedBefore);
#endif
        return 0;
    }

    sample* dst = g_host.audioBuf[g_host.audioBufIndex];
    if (!dst) {
#ifdef EMU_LOGS_ENABLED
        msx_host_audio_note_write(length, 0, queuedBefore);
#endif
        return 0;
    }

    std::memcpy(dst, data, length * sizeof(sample));
    g_host.audioBufIndex = (g_host.audioBufIndex + 1) % kAudioBufCount;

    M5Cardputer.Speaker.playRaw(
        dst,
        length,
        (uint32_t)kAudioRate,
        false,
        1,
        kAudioChannel,
        false
    );

#ifdef EMU_LOGS_ENABLED
    msx_host_audio_note_write(length, length, queuedBefore);
#endif
    return length;
}

extern "C" void PlayAllSound(int uSec)
{
    using namespace msx;

    g_host.pendingUsec += uSec;
    while (g_host.pendingUsec >= (uint64_t)kAudioChunk * 1000000 / kAudioRate) {
#ifdef EMU_LOGS_ENABLED
        const unsigned int freeBefore = GetFreeAudio();
        const unsigned int played = RenderAndPlayAudio(kAudioChunk);
        msx_host_audio_note_render(kAudioChunk, played, freeBefore);
#else
        RenderAndPlayAudio(kAudioChunk);
#endif
        g_host.pendingUsec -= (uint64_t)kAudioChunk * 1000000 / kAudioRate;
    }
}
