#if !defined(_MSC_VER) && !defined(ESP_PLATFORM)
#include <sched.h>
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <boolean.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#pragma pack(1)
#endif

#include <libretro.h>

#include "Bios.h"
#include "Cartridge.h"
#include "Database.h"
#include "Maria.h"
#include "Palette.h"
#include "Pokey.h"
#include "Region.h"
#include "ProSystem.h"
#include "Tia.h"
#include "Memory.h"
#include "BupChip.h"

#ifdef _3DS
extern void* linearMemAlign(size_t size, size_t alignment);
extern void linearFree(void* mem);
#endif

static int videoWidth                   = 320;
static int videoHeight                  = 240;
static uint16_t *display_palette16      = NULL;
static uint8_t keyboard_data[17]        = {0};

static bool persistent_data             = false;

/* Required buffer size is exactly TIA_BUFFER_SIZE,
 * but round up to nearest multiple of 128 for
 * peace of mind... */
#define AUDIO_SAMPLE_BUFFER_SIZE ((TIA_BUFFER_SIZE + 0x7F) & ~0x7F)
static uint8_t *pokeyMixBuffer          = NULL;
static int16_t *audioOutBuffer          = NULL;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;

static bool libretro_supports_bitmasks = false;

const uint16_t* a7800_core_get_palette16(void)
{
   return display_palette16;
}

static void* libretro_Alloc(size_t size)
{
   return malloc(size);
}

static bool libretro_EnsureBuffers(void)
{
   if(!display_palette16)
   {
      display_palette16 = (uint16_t*)libretro_Alloc(256 * sizeof(uint16_t));
      if(!display_palette16)
         return false;

      memset(display_palette16, 0, 256 * sizeof(uint16_t));
   }

   if(!pokeyMixBuffer)
   {
      pokeyMixBuffer = (uint8_t*)libretro_Alloc(
            AUDIO_SAMPLE_BUFFER_SIZE * sizeof(uint8_t));
      if(!pokeyMixBuffer)
         return false;
   }

   if(!audioOutBuffer)
   {
      audioOutBuffer = (int16_t*)libretro_Alloc(
            (AUDIO_SAMPLE_BUFFER_SIZE << 1) * sizeof(int16_t));
      if(!audioOutBuffer)
         return false;
   }

   return true;
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
}

#define BLIT_VIDEO_BUFFER(typename_t, src, palette, width, height, pitch, dst) \
   {                                                                           \
      typename_t *surface = (typename_t*)dst;                                  \
      uint32_t x, y;                                                           \
                                                                               \
      for(y = 0; y < height; y++)                                              \
      {                                                                        \
         typename_t *surface_ptr = surface;                                    \
         const uint8_t *src_ptr  = src;                                        \
                                                                               \
         for(x = 0; x < width; x++)                                            \
            *(surface_ptr++) = *(palette + *(src_ptr++));                      \
                                                                               \
         surface += pitch;                                                     \
         src     += width;                                                     \
      }                                                                        \
   }

static void display_ResetPalette(void)
{
   unsigned index;

   if(!display_palette16)
      return;

   for(index = 0; index < 256; index++)
   {
      uint32_t r = palette_data[(index * 3) + 0] << 16;
      uint32_t g = palette_data[(index * 3) + 1] << 8;
      uint32_t b = palette_data[(index * 3) + 2];
      display_palette16[index] = ((r & 0xF80000) >> 8) |
                                 ((g & 0x00F800) >> 5) |
                                 ((b & 0x0000F8) >> 3);
   }
}

static short sound_Lerp(short a, short b, float t)
{
   return (short)floorf((float)a + (float)(b - a) * t + 0.5f);
}

static void sound_ResampleBupChip(const short* source, short* target, int length)
{
   int targetIndex;
   uint32_t bupchipBufferSize = CORETONE_BUFFER_SAMPLES * 4;

   for(targetIndex = 0; targetIndex < length; targetIndex++)
   {
      float t;
      int channel;
      float sourceIndex = (float)targetIndex / (float)length * (float)bupchipBufferSize;
      uint32_t sourceLo = (uint32_t)floorf(sourceIndex);
      uint32_t sourceHi = (uint32_t)ceilf(sourceIndex);

      if(sourceHi >= bupchipBufferSize)
         sourceHi = bupchipBufferSize - 1;

      t = sourceIndex - (float)sourceLo;

      for(channel = 0; channel < 2; channel++)
      {
         int sample = sound_Lerp(
               source[sourceLo * 2 + channel],
               source[sourceHi * 2 + channel],
               t);

         sample += target[targetIndex * 2 + channel];

         if(sample > INT16_MAX)
            sample = INT16_MAX;
         else if(sample < INT16_MIN)
            sample = INT16_MIN;

         target[targetIndex * 2 + channel] = sample;
      }
   }
}

static void sound_Store(void)
{
   uint8_t *tia_samples_buf = tia_buffer;
   int16_t *audio_out_buf   = audioOutBuffer;
   size_t i, j;

   if(!tia_samples_buf || !audioOutBuffer)
      return;

   if(cartridge_pokey)
   {
      uint8_t *pokey_samples_buf = pokey_buffer;
      uint8_t *pokey_mix_buf     = pokeyMixBuffer;

      if(!pokey_samples_buf || !pokey_mix_buf)
         return;

      for(j = 0; j < tia_size; j++)
         *(pokey_mix_buf++) = (*(tia_samples_buf++) + *(pokey_samples_buf++)) >> 1;

      tia_samples_buf = pokeyMixBuffer;
   }

   for(i = 0; i < tia_size; i++)
   {
      int16_t sample_16 = (int16_t)(*(tia_samples_buf++) << 8);
      *(audio_out_buf++) = sample_16;
      *(audio_out_buf++) = sample_16;
   }

   if(cartridge_bupchip && bupchip_buffer)
      sound_ResampleBupChip(bupchip_buffer, audioOutBuffer, tia_size);

   audio_batch_cb(audioOutBuffer, tia_size);
}

static void update_input(void)
{
   unsigned i, j;
   unsigned joypad_bits[2];

   input_poll_cb();

   if(libretro_supports_bitmasks)
   {
      for(j = 0; j < 2; j++)
         joypad_bits[j] = input_state_cb(j, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
   }
   else
   {
      for(j = 0; j < 2; j++)
      {
         joypad_bits[j] = 0;
         for(i = 0; i < (RETRO_DEVICE_ID_JOYPAD_R3 + 1); i++)
            joypad_bits[j] |= input_state_cb(j, RETRO_DEVICE_JOYPAD, 0, i) ? (1U << i) : 0;
      }
   }

   keyboard_data[0]  = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)  ? 1 : 0;
   keyboard_data[1]  = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)   ? 1 : 0;
   keyboard_data[2]  = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)   ? 1 : 0;
   keyboard_data[3]  = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_UP)     ? 1 : 0;
   keyboard_data[4]  = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_B)      ? 1 : 0;
   keyboard_data[5]  = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_A)      ? 1 : 0;

   keyboard_data[6]  = joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)  ? 1 : 0;
   keyboard_data[7]  = joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)   ? 1 : 0;
   keyboard_data[8]  = joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)   ? 1 : 0;
   keyboard_data[9]  = joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_UP)     ? 1 : 0;
   keyboard_data[10] = joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_B)      ? 1 : 0;
   keyboard_data[11] = joypad_bits[1] & (1 << RETRO_DEVICE_ID_JOYPAD_A)      ? 1 : 0;

   keyboard_data[12] = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_X)      ? 1 : 0;
   keyboard_data[13] = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_SELECT) ? 1 : 0;
   keyboard_data[14] = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_START)  ? 1 : 0;
   keyboard_data[15] = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_L)      ? 1 : 0;
   keyboard_data[16] = joypad_bits[0] & (1 << RETRO_DEVICE_ID_JOYPAD_R)      ? 1 : 0;
}

/************************************
 * libretro implementation
 ************************************/

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name = "ProSystem";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version  = "1.3e" GIT_VERSION;
   info->need_fullpath    = false;
   info->valid_extensions = "a78";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = (cartridge_region == REGION_NTSC) ? 60 : 50;
   info->timing.sample_rate    = (prosystem_frequency * prosystem_scanlines) << 1;
   info->geometry.base_width   = videoWidth;
   info->geometry.base_height  = (cartridge_region == REGION_NTSC) ? 223 : 272;
   info->geometry.max_width    = 320;
   info->geometry.max_height   = 292;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

bool retro_load_game(const struct retro_game_info *info)
{
   enum retro_pixel_format fmt;
   char biospath[512];
   const char *system_directory_c = NULL;
   bool reset_ok = false;
#ifdef _WIN32
   char slash = '\\';
#else
   char slash = '/';
#endif

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Console Reset" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Console Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Console Pause" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Left Difficulty" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Right Difficulty" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "(Dual Stick) P2 X-Axis" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "(Dual Stick) P2 Y-Axis" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "2" },
      { 0 },
   };

   if(!info)
      return false;

   if(!libretro_EnsureBuffers())
   {
#ifdef A7800_LOGS
      EMU_LOG("[A7800][CORE] audio/video buffer allocation failed\n");
#endif
      return false;
   }

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   fmt = RETRO_PIXEL_FORMAT_RGB565;
   if(!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if(log_cb)
         log_cb(RETRO_LOG_INFO, "[ProSystem]: RGB565 is not supported.\n");
      return false;
   }

   memset(keyboard_data, 0, sizeof(keyboard_data));
   keyboard_data[15] = 1;
   keyboard_data[16] = 0;

   persistent_data = true;

   if(!cartridge_Load(persistent_data, (const uint8_t*)info->data, info->size))
   {
#ifdef A7800_LOGS
      EMU_LOG("[A7800][CORE] cartridge_Load failed, size=%u\n", info->size);
#endif
      return false;
   }

   database_Load(cartridge_digest);

   environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory_c);

   if(cartridge_region == REGION_PAL)
      sprintf(biospath, "%s%c%s", system_directory_c, slash, "7800 BIOS (E).rom");
   else
      sprintf(biospath, "%s%c%s", system_directory_c, slash, "7800 BIOS (U).rom");

   if(bios_Load(biospath))
      bios_enabled = true;

   reset_ok = prosystem_Reset();
   if(!reset_ok && bios_enabled)
   {
#ifdef A7800_LOGS
      EMU_LOG("[A7800][CORE] retrying without BIOS due to low memory\n");
#endif
      bios_Release();
      reset_ok = prosystem_Reset();
   }

   if(!reset_ok)
   {
#ifdef A7800_LOGS
      EMU_LOG("[A7800][CORE] prosystem_Reset failed\n");
#endif
      bios_Release();
      cartridge_Release(persistent_data);
      persistent_data = false;
      return false;
   }

   display_ResetPalette();
   return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   (void)game_type;
   (void)info;
   (void)num_info;
   return false;
}

void retro_unload_game(void)
{
   prosystem_Close(persistent_data);
   bios_Release();
   persistent_data = false;
}

unsigned retro_get_region(void)
{
   return cartridge_region == REGION_NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void *retro_get_memory_data(unsigned id)
{
   if(id == RETRO_MEMORY_SYSTEM_RAM)
      return memory_ram;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   if(id == RETRO_MEMORY_SYSTEM_RAM)
      return MEMORY_SIZE;
   return 0;
}

void retro_init(void)
{
   struct retro_log_callback log;
   unsigned level = 5;

   if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

   if(environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   libretro_EnsureBuffers();
}

void retro_deinit(void)
{
   libretro_supports_bitmasks = false;

   if(display_palette16)
   {
      free(display_palette16);
      display_palette16 = NULL;
   }

   if(pokeyMixBuffer)
   {
      free(pokeyMixBuffer);
      pokeyMixBuffer = NULL;
   }

   if(audioOutBuffer)
   {
      free(audioOutBuffer);
      audioOutBuffer = NULL;
   }
}

void retro_reset(void)
{
   prosystem_Reset();
}

static INLINE uint32_t Rect_GetHeight(struct Rects *rect)
{
   return (rect->bottom - rect->top) + 1;
}

void retro_run(void)
{
   const uint8_t *buffer = NULL;
   uint32_t video_pitch  = 320;

   (void)video_pitch;

   update_input();

   prosystem_ExecuteFrame(keyboard_data);

   videoWidth  = Rect_GetLength(&maria_visibleArea);
   videoHeight = Rect_GetHeight(&maria_visibleArea);
   buffer      = maria_surface + ((maria_visibleArea.top - maria_displayArea.top) * Rect_GetLength(&maria_visibleArea));

   video_cb(buffer, videoWidth, videoHeight, videoWidth);
   sound_Store();
}
