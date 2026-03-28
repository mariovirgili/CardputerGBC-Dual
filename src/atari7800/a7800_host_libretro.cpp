#include "a7800_host_libretro.h"

#include <string.h>

#include "a7800_audio.h"
#include "a7800_video.h"

extern "C" const uint16_t* a7800_core_get_palette16(void);

static A7800HostState* s_host = nullptr;

static uint32_t a7800_build_joypad_mask(const A7800InputState& input)
{
    uint32_t mask = 0;

    if (input.left) {
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    }
    if (input.right) {
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    }
    if (input.up) {
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    }
    if (input.down) {
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    }
    if (input.fire1) {
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
    }
    if (input.fire2) {
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
    }
    if (input.reset) {
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_X;
    }
    if (input.select) {
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
    }
    if (input.pause) {
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
    }

    return mask;
}

static const char* a7800_get_option_value(const char* key)
{
    if (!key) {
        return nullptr;
    }
    if (strcmp(key, "prosystem_color_depth") == 0) {
        return "16bit";
    }
    if (strcmp(key, "prosystem_low_pass_filter") == 0) {
        return "disabled";
    }
    if (strcmp(key, "prosystem_low_pass_range") == 0) {
        return "60";
    }
    if (strcmp(key, "prosystem_gamepad_dual_stick_hack") == 0) {
        return "disabled";
    }
    return nullptr;
}

static bool a7800_environment_cb(unsigned cmd, void* data)
{
    if (!s_host) {
        return false;
    }

    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *(unsigned*)data = 1;
            return true;

        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
            return true;

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data = s_host->systemDirectory;
            return true;

        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            retro_variable* variable = (retro_variable*)data;
            variable->value = a7800_get_option_value(variable->key);
            return variable->value != nullptr;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data = false;
            return true;

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const retro_pixel_format format = *(const retro_pixel_format*)data;
            if (format == RETRO_PIXEL_FORMAT_RGB565) {
                s_host->pixelFormat = format;
                return true;
            }
            return false;
        }

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            return false;

        default:
            return false;
    }
}

static void a7800_video_refresh_cb(const void* data, unsigned width, unsigned height, size_t pitch)
{
    if (!s_host || !data) {
        return;
    }

    s_host->frameWidth = width;
    s_host->frameHeight = height;
    s_host->framePitch = pitch;
    s_host->videoFrameCount++;

    a7800_video_submit_frame(
        data,
        width,
        height,
        pitch,
        a7800_core_get_palette16(),
        true,
        s_host->isPal
    );
}

static void a7800_audio_sample_cb(int16_t left, int16_t right)
{
    const int16_t sample[2] = {left, right};
    a7800_audio_submit_batch(sample, 1);
    if (s_host) {
        s_host->audioFrameCount++;
    }
}

static size_t a7800_audio_batch_cb(const int16_t* data, size_t frames)
{
    a7800_audio_submit_batch(data, frames);
    if (s_host) {
        s_host->audioFrameCount += frames;
    }
    return frames;
}

static void a7800_input_poll_cb(void)
{
}

static int16_t a7800_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;

    if (!s_host || port > 1 || device != RETRO_DEVICE_JOYPAD) {
        return 0;
    }

    const uint32_t mask = s_host->joypadMask[port];
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        return (int16_t)mask;
    }

    return (mask & (1u << id)) != 0 ? 1 : 0;
}

bool a7800_host_init(A7800HostState* state)
{
    if (!state) {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->systemDirectory = "/sd";
    state->pixelFormat = RETRO_PIXEL_FORMAT_RGB565;
    state->aspectRatio = 4.0f / 3.0f;

    s_host = state;

    retro_set_environment(a7800_environment_cb);
    retro_set_video_refresh(a7800_video_refresh_cb);
    retro_set_audio_sample(a7800_audio_sample_cb);
    retro_set_audio_sample_batch(a7800_audio_batch_cb);
    retro_set_input_poll(a7800_input_poll_cb);
    retro_set_input_state(a7800_input_state_cb);

    retro_init();

    state->initialized = true;
    return true;
}

bool a7800_host_load_game(A7800HostState* state,
                          const uint8_t* romData,
                          size_t romLen,
                          const char* romName)
{
    if (!state || !state->initialized || !romData || romLen == 0) {
        return false;
    }

    retro_game_info info = {};
    info.path = romName;
    info.data = romData;
    info.size = romLen;
    info.meta = nullptr;

    state->romData = romData;
    state->romLen = romLen;
    state->romName = romName;

    if (!retro_load_game(&info)) {
        return false;
    }

    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
    retro_get_system_av_info(&state->avInfo);

    state->fps = state->avInfo.timing.fps;
    state->sampleRate = (unsigned)state->avInfo.timing.sample_rate;
    state->aspectRatio = state->avInfo.geometry.aspect_ratio > 0.0f
                             ? state->avInfo.geometry.aspect_ratio
                             : (4.0f / 3.0f);
    state->isPal = retro_get_region() == RETRO_REGION_PAL || state->fps < 55.0;
    state->loaded = true;
    return true;
}

void a7800_host_run_frame(A7800HostState* state, const A7800InputState* input)
{
    if (!state || !state->loaded) {
        return;
    }

    if (input) {
        state->input = *input;
    } else {
        memset(&state->input, 0, sizeof(state->input));
    }

    state->joypadMask[0] = a7800_build_joypad_mask(state->input);
    state->joypadMask[1] = 0;

    retro_run();
}

const retro_system_av_info* a7800_host_get_av_info(const A7800HostState* state)
{
    return state ? &state->avInfo : nullptr;
}

bool a7800_host_is_pal(const A7800HostState* state)
{
    return state ? state->isPal : false;
}

double a7800_host_get_fps(const A7800HostState* state)
{
    return state ? state->fps : 60.0;
}

unsigned a7800_host_get_sample_rate(const A7800HostState* state)
{
    return state ? state->sampleRate : 44100u;
}

void a7800_host_unload_game(A7800HostState* state)
{
    if (!state || !state->loaded) {
        return;
    }

    retro_unload_game();
    state->loaded = false;
}

void a7800_host_shutdown(A7800HostState* state)
{
    if (!state) {
        return;
    }

    if (state->loaded) {
        a7800_host_unload_game(state);
    }

    if (state->initialized) {
        retro_deinit();
        state->initialized = false;
    }

    if (s_host == state) {
        s_host = nullptr;
    }
}
