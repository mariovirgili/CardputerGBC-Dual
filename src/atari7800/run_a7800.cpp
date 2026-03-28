#include "run_a7800.h"

#include <Arduino.h>

#include <Preferences.h>

#include <cmath>

#include "a7800_audio.h"
#include "a7800_host_libretro.h"
#include "a7800_input.h"
#include "a7800_video.h"
#include "core/core/Maria.h"
#include "cardputer/CardputerView.h"
#include "esp_timer.h"
#include "share/display_target.h"
#include "share/emu_controls.h"
#include "share/game_save.h"
#include "share/utils.h"

static void a7800_request_quit_to_launcher(void)
{
    Preferences prefs;
    prefs.begin("cardputer_emu", false);
    prefs.putBool("quit_game", true);
    prefs.end();

    while (share::gameIsSaving()) {
        delay(1);
    }

    esp_restart();
}

void run_a7800(const uint8_t* romData, size_t romLen, const char* romName)
{
    const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);

    {
        CardputerView display;
        display.initialize();
        if (useExternal) {
            display.topBar("A7800 ON EXTERNAL TFT", false, false);
            display.showControlBindings(
                share::emuControlActionLabels(share::EmuProfile::A7800),
                share::emuControlKeyLabels(share::EmuProfile::A7800),
                "GO / HOLD ESC = QUIT"
            );
        } else if (!emu_is_aux_screen_locked()) {
            a7800_video_show_external_info(romName);
        }
    }

    A7800HostState host = {};
    a7800_input_init();

    if (!a7800_host_init(&host)) {
        printf("[A7800] host init failed\n");
        return;
    }

    if (!a7800_host_load_game(&host, romData, romLen, romName)) {
        printf("[A7800] retro_load_game failed\n");
        a7800_host_shutdown(&host);
        return;
    }

    const retro_system_av_info* avInfo = a7800_host_get_av_info(&host);
    const double targetFps = a7800_host_get_fps(&host);
    const unsigned sampleRate = a7800_host_get_sample_rate(&host);
    const bool isPal = a7800_host_is_pal(&host);

    a7800_video_init(
        targetFps,
        avInfo ? avInfo->geometry.base_width : 320,
        avInfo ? avInfo->geometry.base_height : (isPal ? 272 : 223),
        avInfo ? avInfo->geometry.aspect_ratio : (4.0f / 3.0f)
    );
    a7800_audio_init(sampleRate);

    printf("[A7800] core loaded, fps=%.2f, rate=%u, base=%ux%u, pal=%d\n",
           targetFps,
           sampleRate,
           avInfo ? avInfo->geometry.base_width : 0,
           avInfo ? avInfo->geometry.base_height : 0,
           isPal ? 1 : 0);

    const uint32_t frameUs = (uint32_t)lround(1000000.0 / (targetFps > 0.0 ? targetFps : 60.0));
    uint64_t nextFrameUs = esp_timer_get_time();
    uint32_t frameCount = 0;
    uint32_t renderCount = 0;
    uint32_t lastLogMs = millis();
    bool quitRequested = false;
    int64_t  s_frameUs = 0; /* accumulated retro_run() wall time */
    bool skipNextVideo = false;
    int  consecSkips = 0;
    /* Guaranteed render floor: always render at least every kRenderFloor frames
     * so the display doesn't go dark when deeply behind schedule. */
    const int kRenderFloor = 7;

    while (!quitRequested) {
        A7800InputState input = {};
        a7800_input_poll(&input);
        if (input.quitRequested) {
            quitRequested = true;
            break;
        }

        /* Tell the video layer AND MARIA whether to skip this frame's render.
         * DMA cycle counts still run normally; only pixel writes are suppressed. */
        a7800_video_set_frame_skip(skipNextVideo);
        maria_skip_render = skipNextVideo;
        if (!skipNextVideo) renderCount++;

        {
            const int64_t t0 = esp_timer_get_time();
            a7800_host_run_frame(&host, &input);
            s_frameUs += (esp_timer_get_time() - t0);
        }

        frameCount++;
        const uint32_t nowMs = millis();
        if (nowMs - lastLogMs >= 1000) {
            const float elapsedMs = (float)(nowMs - lastLogMs);
            const float fps  = (frameCount  * 1000.0f) / elapsedMs;
            const float rfps = (renderCount * 1000.0f) / elapsedMs;

            /* Per-frame average time split between video and CPU+MARIA */
            int64_t videoUs = 0; uint32_t videoCount = 0;
            a7800_video_get_and_reset_stats(&videoUs, &videoCount);
            const int32_t avgFrameMs  = frameCount  ? (int32_t)(s_frameUs / 1000 / frameCount) : 0;
            const int32_t avgVideoMs  = videoCount  ? (int32_t)(videoUs   / 1000 / videoCount) : 0;
            const int32_t avgCpuMs    = avgFrameMs - avgVideoMs;

            printf("[A7800] EMU %.1f fps | RENDER %.1f fps | frame %ldms (cpu %ldms + vid %ldms) | HEAP %u\n",
                   fps, rfps,
                   (long)avgFrameMs, (long)avgCpuMs, (long)avgVideoMs,
                   esp_get_free_heap_size());
            frameCount  = 0;
            renderCount = 0;
            s_frameUs   = 0;
            lastLogMs   = nowMs;
        }

        nextFrameUs += frameUs;
        const int64_t nowUs = (int64_t)esp_timer_get_time();
        const int64_t lateness = nowUs - (int64_t)nextFrameUs;

        /* Prevent unbounded timing debt — if more than 3 frames behind,
         * reset the clock so the skip logic doesn't spiral. */
        if (lateness > (int64_t)(frameUs * 3)) {
            nextFrameUs = (uint64_t)nowUs;
        }

        /* Dynamic skip limit: allow more consecutive skips the further
         * behind we are, but never starve the display past kRenderFloor. */
        int maxConsecSkips;
        if      (lateness > (int64_t)(frameUs * 3)) maxConsecSkips = 5;
        else if (lateness > (int64_t)(frameUs))     maxConsecSkips = 3;
        else                                         maxConsecSkips = 1;

        /* Guarantee a render at least every kRenderFloor frames. */
        const bool mustRender = (consecSkips >= kRenderFloor);

        if (!mustRender && lateness > 0 && consecSkips < maxConsecSkips) {
            /* Behind schedule and haven't hit skip limit yet */
            skipNextVideo = true;
            consecSkips++;
            taskYIELD();
        } else {
            /* On time, hit skip limit, or forced by render floor */
            skipNextVideo = false;
            consecSkips = 0;
            if (lateness <= 0) {
                share::sleep_until_us(nextFrameUs);
            } else {
                taskYIELD();
            }
        }
    }

    a7800_host_unload_game(&host);
    a7800_host_shutdown(&host);
    a7800_audio_shutdown();
    a7800_video_shutdown();

    if (quitRequested) {
        a7800_request_quit_to_launcher();
    }
}
