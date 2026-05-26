#pragma once
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "genesis/gwenesis/bus/gwenesis_bus.h"

// Constantes audio
#ifndef MD_AUDIO_SAMPLE_RATE
#define MD_AUDIO_SAMPLE_RATE 44000
#endif
#ifndef MD_AUDIO_WALLCLOCK_CHUNK_CAP
#define MD_AUDIO_WALLCLOCK_CHUNK_CAP 0
#endif

static constexpr int AUDIO_SR = MD_AUDIO_SAMPLE_RATE;   // Hz
static constexpr bool AUDIO_STEREO = false;
static constexpr int AUDIO_POOL = 4;
static constexpr int AUDIO_Q_DEPTH = 4;
static constexpr int AUDIO_CHUNK_NTSC = (AUDIO_SR + GWENESIS_REFRESH_RATE_NTSC / 2) / GWENESIS_REFRESH_RATE_NTSC;
static constexpr int AUDIO_CHUNK_PAL = (AUDIO_SR + GWENESIS_REFRESH_RATE_PAL / 2) / GWENESIS_REFRESH_RATE_PAL;
static constexpr int AUDIO_CHUNK_NOMINAL_CAP = (AUDIO_CHUNK_PAL > AUDIO_CHUNK_NTSC) ? AUDIO_CHUNK_PAL : AUDIO_CHUNK_NTSC;
static constexpr int AUDIO_CHUNK_CAP = (MD_AUDIO_WALLCLOCK_CHUNK_CAP > AUDIO_CHUNK_NOMINAL_CAP) ? MD_AUDIO_WALLCLOCK_CHUNK_CAP : AUDIO_CHUNK_NOMINAL_CAP;
static constexpr int AUDIO_CORE_CHUNK_CAP = ((LINES_PER_FRAME_PAL * VDP_CYCLES_PER_LINE) + GWENESIS_AUDIO_DIVISOR_PAL - 1) / GWENESIS_AUDIO_DIVISOR_PAL;

typedef void (*GenesisAudioSink)(int16_t* pcm, size_t n_samples, int sample_rate);

struct AudioMsg {
  int16_t* buf;
  size_t   n;    // samples
};

typedef struct {
  uint32_t mclk;   // horloge master
  uint8_t  port;   // 0,2: addr ; 1,3: data
  uint8_t  value;
} YmWrite;

#ifdef __cplusplus
extern "C" {
#endif

// Buffers PSG
extern int16_t* gwenesis_sn76489_buffer;
extern int      sn76489_index;
extern int      sn76489_clock;

// Buffers YM2612
extern int16_t* gwenesis_ym2612_buffer;
extern int      ym2612_index;
extern int      ym2612_clock;
extern volatile int g_ym_target_clock;

extern uint8_t genesis_audio_volume;

// API audio
void genesis_sound_configure_timing(int refresh_rate, int core_sample_rate, int core_divisor, int lines_per_frame);
int genesis_sound_get_refresh_rate(void);
int genesis_sound_get_core_rate(void);
int genesis_sound_get_core_samples_per_frame(void);
int genesis_sound_get_output_samples_per_frame(void);
void genesis_alloc_audio_buffers(void);
void genesis_sound_init();
void genesis_sound_submit_frame(uint32_t frame_elapsed_us);
void genesis_sound_shutdown(void);

// API YM
void genesis_sound_ym_init(void);
void genesis_sound_ym_start(void);
void genesis_sound_ym_stop(void);
void genesis_sound_ym_set_target_clock(int target);
int  genesis_sound_ym_get_target_clock(void);

#ifdef __cplusplus
} // extern "C"
#endif
