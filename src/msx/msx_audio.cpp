#pragma GCC optimize ("Os")

#include "msx_host_internal.h"

#include <cstdlib>
#include <cstring>

#include "cardputer/CardputerAudio.h"
#include "esp_heap_caps.h"

extern "C" unsigned int InitAudio(unsigned int Rate, unsigned int)
{
    using namespace msx;

    if (!g_host.audioBuf) {
        g_host.audioBuf = (sample**)std::calloc(kAudioBufCount, sizeof(sample*));
    }
    if (!g_host.audioBuf) return 0;

    cardputer_audio::beginSpeaker(Rate, false, 512, 8, 60, "msx", 4, 0);
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
    return Rate;
}

extern "C" void TrashAudio(void)
{
    M5Cardputer.Speaker.stop(msx::kAudioChannel);

    if (msx::g_host.audioBuf) {
        for (int i = 0; i < msx::kAudioBufCount; ++i) {
            heap_caps_free(msx::g_host.audioBuf[i]);
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

    const size_t depth = M5Cardputer.Speaker.isPlaying(kAudioChannel);
    if (depth > 1) {
        cardputer_audio::recordQueueDiag(length, kAudioRate, false, kAudioChannel, depth, false, false, true);
        return 0;
    }

    sample* dst = g_host.audioBuf[g_host.audioBufIndex];
    if (!dst) {
        cardputer_audio::recordQueueDiag(length, kAudioRate, false, kAudioChannel, depth, false, true, false);
        return 0;
    }

    std::memcpy(dst, data, length * sizeof(sample));
    g_host.audioBufIndex = (g_host.audioBufIndex + 1) % kAudioBufCount;

    const bool ok = M5Cardputer.Speaker.playRaw(
        dst,
        length,
        (uint32_t)kAudioRate,
        false,
        1,
        kAudioChannel,
        false
    );

    cardputer_audio::recordQueueDiag(length, kAudioRate, false, kAudioChannel, depth, ok, false, false);
    return ok ? length : 0;
}

extern "C" void PlayAllSound(int uSec)
{
    using namespace msx;

    g_host.pendingUsec += uSec;
    while (g_host.pendingUsec >= (uint64_t)kAudioChunk * 1000000 / kAudioRate) {
        RenderAndPlayAudio(kAudioChunk);
        g_host.pendingUsec -= (uint64_t)kAudioChunk * 1000000 / kAudioRate;
    }
}
