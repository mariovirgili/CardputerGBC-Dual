#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "libretro.h"
#include "../videopac_trace.h"

static uint16_t* s_video_buf = NULL;
static const uint8_t* s_indexed_video_buf = NULL;
static int s_video_w = 0;
static int s_video_h = 0;
static int s_video_pitch_pixels = 0;

#define MAX_AUDIO_FRAMES 768
static int16_t* s_audio_buf = NULL;
static size_t s_audio_frames = 0;

static bool s_joy_up = false;
static bool s_joy_down = false;
static bool s_joy_left = false;
static bool s_joy_right = false;
static bool s_joy_action = false;
static bool s_retro_keys[512] = {false};
static bool s_core_loaded = false;
static bool s_audio_seen = false;
static bool s_logged_pixel_format = false;

static const char* s_system_dir = "/sd/bios/videopac";
static const char* s_bios_name = "o2rom.bin";
static bool s_crop_overscan =
#if defined(FRONTEND_SUPPORTS_INDEXED_VIDEO)
    false;
#else
    true;
#endif

extern "C" {
    void retro_init(void);
    void retro_deinit(void);
    void retro_unload_game(void);
    void retro_set_environment(retro_environment_t);
    void retro_set_video_refresh(retro_video_refresh_t);
    void retro_set_audio_sample(retro_audio_sample_t);
    void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
    void retro_set_input_poll(retro_input_poll_t);
    void retro_set_input_state(retro_input_state_t);
    bool retro_load_game(const struct retro_game_info *game);
    void retro_run(void);
    void retro_get_system_av_info(struct retro_system_av_info *info);
    void retro_o2em_set_bios_image(const uint8_t *data, size_t size);
    void o2em_vdc_get_palette565(uint16_t out_palette[256]);
    void vpp_set_external_native_mode(int enabled);
    int vpp_render_external_line_rgb565(uint16_t *out_line,
                                        int dst_w,
                                        int dst_h,
                                        int dy,
                                        const uint8_t *base_line,
                                        int src_w,
                                        int src_h,
                                        const uint16_t palette[256]);
}

static void log_printf_cb(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            if (data && (*static_cast<enum retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_RGB565)) {
                if (!s_logged_pixel_format) {
                    videopac_trace_mark("bridge", "env pixel_format=RGB565 accepted");
                    s_logged_pixel_format = true;
                }
                return true;
            }
            videopac_trace_mark("bridge", "env pixel_format rejected");
            return false;

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            if (data) {
                *static_cast<const char**>(data) = s_system_dir;
                return true;
            }
            return false;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            if (data) {
                static struct retro_log_callback log = { log_printf_cb };
                *static_cast<struct retro_log_callback*>(data) = log;
                return true;
            }
            return false;

        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            if (!data) return false;
            auto* var = static_cast<struct retro_variable*>(data);
            if (!var || !var->key) return false;

            if (strcmp(var->key, "o2em_bios") == 0) {
                var->value = s_bios_name ? s_bios_name : "o2rom.bin";
                return true;
            }
            if (strcmp(var->key, "o2em_crop_overscan") == 0) {
                var->value = s_crop_overscan ? "enabled" : "disabled";
                return true;
            }
            if (strcmp(var->key, "o2em_mix_frames") == 0) {
                var->value = "disabled";
                return true;
            }
            if (strcmp(var->key, "o2em_audio_volume") == 0) {
                var->value = "70";
                return true;
            }
            if (strcmp(var->key, "o2em_low_pass_filter") == 0) {
                var->value = "disabled";
                return true;
            }
            if (strcmp(var->key, "o2em_low_pass_range") == 0) {
                var->value = "60";
                return true;
            }
            var->value = NULL;
            return false;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            if (data) {
                *static_cast<bool*>(data) = false;
                return true;
            }
            return false;

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;

        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;

        default:
            return false;
    }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    static unsigned last_width = 0;
    static unsigned last_height = 0;
    static size_t last_pitch = 0;

#if defined(FRONTEND_SUPPORTS_INDEXED_VIDEO)
    s_indexed_video_buf = static_cast<const uint8_t*>(data);
    s_video_buf = NULL;
    s_video_w = (int)width;
    s_video_h = (int)height;
    s_video_pitch_pixels = (int)pitch;
#else
    s_video_buf = (uint16_t*)data;
    s_indexed_video_buf = NULL;
    s_video_w = (int)width;
    s_video_h = (int)height;
    s_video_pitch_pixels = (int)(pitch / sizeof(uint16_t));
#endif

    if (width != last_width || height != last_height || pitch != last_pitch) {
        last_width = width;
        last_height = height;
        last_pitch = pitch;
        videopac_trace_printf("bridge", "video_cb buffer=%p size=%ux%u pitch_bytes=%u",
                              data,
                              static_cast<unsigned>(width),
                              static_cast<unsigned>(height),
                              static_cast<unsigned>(pitch));
    }
}

static void audio_cb(int16_t left, int16_t right)
{
    (void)left;
    (void)right;
}

static void copy_audio_and_drop_dc(int16_t* dst, const int16_t* src, size_t frames)
{
    const size_t samples = frames * 2;
    if (!dst || !src || samples == 0) {
        return;
    }

    int16_t minSample = 32767;
    int16_t maxSample = -32768;
    for (size_t i = 0; i < samples; ++i) {
        const int16_t sample = src[i];
        if (sample < minSample) minSample = sample;
        if (sample > maxSample) maxSample = sample;
    }

    if (static_cast<int32_t>(maxSample) - static_cast<int32_t>(minSample) <= 64) {
        memset(dst, 0, samples * sizeof(int16_t));
        return;
    }

    memcpy(dst, src, samples * sizeof(int16_t));
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    if (!s_audio_buf) {
        return 0;
    }

    if (frames > MAX_AUDIO_FRAMES) {
        videopac_trace_printf("bridge", "audio_batch clipped frames=%u max=%u",
                              static_cast<unsigned>(frames),
                              static_cast<unsigned>(MAX_AUDIO_FRAMES));
        frames = MAX_AUDIO_FRAMES;
    }
    copy_audio_and_drop_dc(s_audio_buf, data, frames);
    s_audio_frames = frames;
    if (!s_audio_seen) {
        s_audio_seen = true;
        videopac_trace_printf("bridge", "audio_batch first frames=%u",
                              static_cast<unsigned>(frames));
    }
    return frames;
}

static void input_poll_cb(void)
{
}

static unsigned ascii_to_retrok(int i)
{
    if (i >= 'a' && i <= 'z') return RETROK_a + (i - 'a');
    if (i >= 'A' && i <= 'Z') return RETROK_a + (i - 'A');
    if (i >= '0' && i <= '9') return RETROK_0 + (i - '0');
    if (i == '\n' || i == '\r') return RETROK_RETURN;
    if (i == ' ') return RETROK_SPACE;
    if (i == '\b') return RETROK_END;
    if (i == '?') return RETROK_QUESTION;
    if (i == '.') return RETROK_PERIOD;
    if (i == '-') return RETROK_MINUS;
    if (i == '*') return RETROK_ASTERISK;
    if (i == '/') return RETROK_SLASH;
    if (i == '=') return RETROK_EQUALS;
    if (i == '+') return RETROK_PLUS;
    return RETROK_UNKNOWN;
}

static int16_t joypad_mask(void)
{
    int16_t mask = 0;
    if (s_joy_action) {
        mask |= (1 << RETRO_DEVICE_ID_JOYPAD_B);
        mask |= (1 << RETRO_DEVICE_ID_JOYPAD_A);
    }
    if (s_joy_up) {
        mask |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
    }
    if (s_joy_down) {
        mask |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    }
    if (s_joy_left) {
        mask |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    }
    if (s_joy_right) {
        mask |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    }
    return mask;
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;

    if (port < 2 && device == RETRO_DEVICE_JOYPAD) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            return joypad_mask();
        }

        switch(id) {
            case RETRO_DEVICE_ID_JOYPAD_UP: return s_joy_up;
            case RETRO_DEVICE_ID_JOYPAD_DOWN: return s_joy_down;
            case RETRO_DEVICE_ID_JOYPAD_LEFT: return s_joy_left;
            case RETRO_DEVICE_ID_JOYPAD_RIGHT: return s_joy_right;
            case RETRO_DEVICE_ID_JOYPAD_B:
            case RETRO_DEVICE_ID_JOYPAD_A: return s_joy_action;
            default: return 0;
        }
    }

    if (device == RETRO_DEVICE_KEYBOARD &&
        id < (sizeof(s_retro_keys) / sizeof(s_retro_keys[0]))) {
        return s_retro_keys[id] ? 1 : 0;
    }

    return 0;
}

extern "C" {

bool o2em_init(const uint8_t* bios_data, size_t bios_size, const char* bios_name,
               const uint8_t* rom_data, size_t rom_size)
{
    videopac_trace_printf("bridge", "init bios=%s bios_size=%u rom_size=%u",
                          bios_name ? bios_name : "(null)",
                          static_cast<unsigned>(bios_size),
                          static_cast<unsigned>(rom_size));
    if (!s_audio_buf) {
        videopac_trace_printf("bridge", "audio_buffer_alloc bytes=%u",
                              static_cast<unsigned>(MAX_AUDIO_FRAMES * 2 * sizeof(*s_audio_buf)));
        s_audio_buf = static_cast<int16_t*>(malloc(MAX_AUDIO_FRAMES * 2 * sizeof(*s_audio_buf)));
        if (!s_audio_buf) {
            videopac_trace_mark("bridge", "audio_buffer_alloc_failed");
            return false;
        }
    }

    s_video_buf = NULL;
    s_indexed_video_buf = NULL;
    s_video_w = 0;
    s_video_h = 0;
    s_video_pitch_pixels = 0;
    s_audio_frames = 0;
    s_audio_seen = false;
    s_logged_pixel_format = false;
    s_bios_name = (bios_name && bios_name[0]) ? bios_name : "o2rom.bin";
    memset(s_retro_keys, 0, sizeof(s_retro_keys));
    retro_o2em_set_bios_image(bios_data, bios_size);

    {
        VideopacTraceScope scope("bridge", "set_callbacks");
        retro_set_environment(env_cb);
        retro_set_video_refresh(video_cb);
        retro_set_audio_sample(audio_cb);
        retro_set_audio_sample_batch(audio_batch_cb);
        retro_set_input_poll(input_poll_cb);
        retro_set_input_state(input_state_cb);
    }

    {
        VideopacTraceScope scope("bridge", "retro_init");
        retro_init();
    }

    struct retro_game_info info = {};
    info.path = "game.bin";
    info.data = rom_data;
    info.size = rom_size;

    bool loaded = false;
    {
        VideopacTraceScope scope("bridge", "retro_load_game");
        loaded = retro_load_game(&info);
    }
    videopac_trace_printf("bridge", "game_loaded=%s", loaded ? "yes" : "no");
    s_core_loaded = loaded;
    return loaded;
}

void o2em_shutdown(void)
{
    videopac_trace_mark("bridge", "shutdown_begin");
    if (s_core_loaded) {
        VideopacTraceScope scope("bridge", "retro_unload_game");
        retro_unload_game();
    }
    {
        VideopacTraceScope scope("bridge", "retro_deinit");
        retro_deinit();
    }
    retro_o2em_set_bios_image(NULL, 0);
    free(s_audio_buf);
    s_audio_buf = NULL;
    s_audio_frames = 0;
    s_core_loaded = false;
    videopac_trace_mark("bridge", "shutdown_end");
}

void o2em_run_frame(void)
{
    if (s_core_loaded) {
        retro_run();
    }
}

void o2em_get_video(uint16_t** out_buffer, int* out_width, int* out_height, int* out_pitch_pixels)
{
    if (out_buffer) *out_buffer = s_video_buf;
    if (out_width) *out_width = s_video_w;
    if (out_height) *out_height = s_video_h;
    if (out_pitch_pixels) *out_pitch_pixels = s_video_pitch_pixels > 0 ? s_video_pitch_pixels : s_video_w;
}

void o2em_get_video_indexed(const uint8_t** out_buffer,
                            int* out_width,
                            int* out_height,
                            int* out_pitch_pixels,
                            uint16_t out_palette[256])
{
    if (out_buffer) *out_buffer = s_indexed_video_buf;
    if (out_width) *out_width = s_video_w;
    if (out_height) *out_height = s_video_h;
    if (out_pitch_pixels) *out_pitch_pixels = s_video_pitch_pixels > 0 ? s_video_pitch_pixels : s_video_w;
    if (out_palette) {
        o2em_vdc_get_palette565(out_palette);
    }
}

void o2em_set_plus_external_native(bool enabled)
{
    vpp_set_external_native_mode(enabled ? 1 : 0);
}

bool o2em_render_plus_external_line(uint16_t* out_line,
                                    int dst_width,
                                    int dst_height,
                                    int dst_y,
                                    const uint8_t* base_line,
                                    int src_width,
                                    int src_height,
                                    const uint16_t palette[256])
{
    return vpp_render_external_line_rgb565(out_line,
                                           dst_width,
                                           dst_height,
                                           dst_y,
                                           base_line,
                                           src_width,
                                           src_height,
                                           palette) != 0;
}

void o2em_get_audio(int16_t** out_buffer, size_t* out_samples)
{
    if (out_buffer) *out_buffer = s_audio_buf;
    if (out_samples) *out_samples = s_audio_frames;
    s_audio_frames = 0;
}

void o2em_set_joystick(bool up, bool down, bool left, bool right, bool action)
{
    s_joy_up = up;
    s_joy_down = down;
    s_joy_left = left;
    s_joy_right = right;
    s_joy_action = action;
}

void o2em_set_key(char key, bool pressed)
{
    const unsigned retrok = ascii_to_retrok((unsigned char)key);
    if (retrok != RETROK_UNKNOWN &&
        retrok < (sizeof(s_retro_keys) / sizeof(s_retro_keys[0]))) {
        s_retro_keys[retrok] = pressed;
    }
}

void o2em_get_system_av_info(double* out_fps, double* out_sample_rate)
{
    struct retro_system_av_info av_info = {};
    retro_get_system_av_info(&av_info);
    if (out_fps) *out_fps = av_info.timing.fps;
    if (out_sample_rate) *out_sample_rate = av_info.timing.sample_rate;
}

} // extern "C"
