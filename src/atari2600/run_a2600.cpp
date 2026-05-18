#pragma GCC optimize ("Os")

#include "run_a2600.h"

#include <cstring>

#include "Cart.hxx"
#include "Console.hxx"
#include "Control.hxx"
#include "Event.hxx"
#include "MD5.hxx"
#include "OSystem.hxx"
#include "Props.hxx"
#include "PropsSet.hxx"
#include "Settings.hxx"
#include "SoundSDL.hxx"
#include "Switches.hxx"
#include "TIA.hxx"
#include "a2600_display.h"
#include "a2600_input.h"
#include "a2600_sound.h"
#include "cardputer/CardputerView.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "share/utils.h"
#include "share/emu_log_cpp.h"

static constexpr int kA2600SampleRate = 31400;
bool RenderFlag = true;

static OSystem* s_osystem = nullptr;
static Settings* s_settings = nullptr;
static Cartridge* s_cartridge = nullptr;
static Console* s_console = nullptr;
static int16_t* s_audioBuffer = nullptr;
static uint16_t *s_palette565 = nullptr;
static uint32_t s_tiaSamplesPerFrame = 0;
static bool s_isPal = false;

static void a2600_shutdown_core()
{
    if (s_console) {
        delete s_console;
        s_console = nullptr;
    }

    if (s_cartridge) {
        delete s_cartridge;
        s_cartridge = nullptr;
    }

    if (s_settings) {
        delete s_settings;
        s_settings = nullptr;
    }

    if (s_osystem) {
        delete s_osystem;
        s_osystem = nullptr;
    }

    free(s_audioBuffer);
    s_audioBuffer = nullptr;
    s_tiaSamplesPerFrame = 0;
}

static bool a2600_init_core(const uint8_t* romData, size_t romLen, const char* romName)
{
    if (!romData || romLen == 0) {
        EMU_LOG("[A2600] invalid ROM buffer\n");
        return false;
    }

    string cartMd5 = MD5((uInt8*)romData, (uInt32)romLen);

    s_osystem = new OSystem();
    if (!s_osystem) {
        EMU_LOG("[A2600] OSystem allocation failed\n");
        return false;
    }

    s_settings = new Settings(s_osystem);
    if (!s_settings) {
        EMU_LOG("[A2600] Settings allocation failed\n");
        a2600_shutdown_core();
        return false;
    }

    s_settings->setValue("sound", true);
    s_settings->setValue("volume", Variant(80u));
    s_settings->setValue("palette", "standard");
    s_settings->setValue("colorloss", false);
    s_settings->setValue("stats", false);
    s_settings->setValue("romloadcount", false);

    Properties props;
    s_osystem->propSet().getMD5(cartMd5, props);
    props.set(Cartridge_MD5, cartMd5);
    props.set(Cartridge_Name, (romName && romName[0]) ? string(romName) : string("Untitled"));

    string cartType = props.get(Cartridge_Type);
    string cartId;

    s_cartridge = Cartridge::create(
        (const uInt8*)romData,
        (uInt32)romLen,
        cartMd5,
        cartType,
        cartId,
        *s_osystem,
        *s_settings
    );
    if (!s_cartridge) {
        EMU_LOG("[A2600] Cartridge creation failed\n");
        a2600_shutdown_core();
        return false;
    }

    s_console = new Console(s_osystem, s_cartridge, props);
    if (!s_console) {
        EMU_LOG("[A2600] Console allocation failed\n");
        a2600_shutdown_core();
        return false;
    }
    s_osystem->myConsole = s_console;

    s_console->initializeVideo();
    s_console->initializeAudio();

    TIA& tia = s_console->tia();
    const int videoWidth = (int)tia.width();
    const int videoHeight = (int)tia.height();
    const float framerate = s_console->getFramerate();

    s_isPal = (videoHeight > 210) || (framerate > 0.0f && framerate <= 55.0f);
    s_tiaSamplesPerFrame = (uint32_t)lround((double)kA2600SampleRate / (double)(framerate > 0.0f ? framerate : 60.0f));
    if (s_tiaSamplesPerFrame == 0) {
        s_tiaSamplesPerFrame = 523;
    }

    const uint32_t* palette = s_console->getPalette(0);
    for (int i = 0; i < 256; ++i) {
        const uint32_t color = palette[i];
        const uint16_t red = (color >> 16) & 0xFF;
        const uint16_t green = (color >> 8) & 0xFF;
        const uint16_t blue = color & 0xFF;
        s_palette565[i] = (uint16_t)(((red << 8) & 0xF800) |
                                     ((green << 3) & 0x07E0) |
                                     (blue >> 3));
    }

    s_audioBuffer = (int16_t*)heap_caps_malloc(
        (size_t)s_tiaSamplesPerFrame * 2 * sizeof(int16_t),
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!s_audioBuffer) {
        EMU_LOG("[A2600] audio buffer alloc failed\n");
        a2600_shutdown_core();
        return false;
    }
    memset(s_audioBuffer, 0, (size_t)s_tiaSamplesPerFrame * 2 * sizeof(int16_t));

    EMU_LOG("[A2600] core initialized, video=%dx%d, fps=%.2f, pal=%d, audio=%u\n",
           videoWidth,
           videoHeight,
           framerate,
           s_isPal ? 1 : 0,
           (unsigned)s_tiaSamplesPerFrame);

    return true;
}

static void a2600_step()
{
    s_console->controller(Controller::Left).update();
    s_console->controller(Controller::Right).update();
    s_console->switches().update();

    TIA& tia = s_console->tia();
    tia.update();

    SoundSDL* sound = static_cast<SoundSDL*>(&s_osystem->sound());
    sound->processFragment(s_audioBuffer, s_tiaSamplesPerFrame);
    a2600_sound_submit_stereo(s_audioBuffer, s_tiaSamplesPerFrame);
}

void run_a2600(const uint8_t* romData, size_t romLen, const char* romName)
{

    s_palette565 = (uint16_t*)heap_caps_malloc(
        256 * sizeof(uint16_t),
        MALLOC_CAP_8BIT
    );
    CardputerView display;
    display.initialize();

    a2600_display_init();
    a2600_display_start();
    a2600_input_init();

    if (!a2600_init_core(romData, romLen, romName)) {
        EMU_LOG("[A2600] init failed\n");
        return;
    }

    a2600_sound_init(kA2600SampleRate);

    const float targetFps = (s_console->getFramerate() > 0.0f) ? s_console->getFramerate() : 60.0f;
    const uint32_t frameUs = (uint32_t)lround(1000000.0 / targetFps);
    uint64_t nextFrameUs = esp_timer_get_time();
#if EMU_LOG_MASTER_ENABLED
    uint32_t frameCount = 0;
    uint32_t lastLogMs = millis();
#endif

    EMU_LOG("[A2600] starting loop @ %.2f FPS\n", targetFps);

    while (true) {
        a2600_input_update(s_osystem->eventHandler().event());
        a2600_step();

        TIA& tia = s_console->tia();
        const bool frameIsPal = s_isPal || tia.height() > 220 || s_console->getFramerate() <= 55.0f;
        a2600_display_submit_frame(
            tia.currentFrameBuffer(),
            (int)tia.width(),
            (int)tia.height(),
            s_palette565,
            frameIsPal
        );

#if EMU_LOG_MASTER_ENABLED
        frameCount++;
        const uint32_t nowMs = millis();
        if (nowMs - lastLogMs >= 1000) {
            const float fps = (frameCount * 1000.0f) / (float)(nowMs - lastLogMs);
            EMU_LOG("[A2600] FPS: %.2f | HEAP %lu\n", fps, (unsigned long)esp_get_free_heap_size());
            frameCount = 0;
            lastLogMs = nowMs;
        }
#endif

        nextFrameUs += frameUs;
        const int64_t nowUs = (int64_t)esp_timer_get_time();
        const int64_t lateness = nowUs - (int64_t)nextFrameUs;

        if (lateness > 0) {
            if (lateness > (int64_t)frameUs) {
                nextFrameUs = (uint64_t)nowUs;
            }
            taskYIELD();
            continue;
        }

        share::sleep_until_us(nextFrameUs);
    }
}
