#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MSX_HOST_VIEW_FIT43 = 0,
    MSX_HOST_VIEW_FULL  = 1,
} msx_host_view_mode_t;

void msx_host_set_game(const uint8_t* romData, unsigned int romSize, const char* romName, const char* romPath);
void msx_host_clear_game(void);

int msx_host_load_bios_for_mode(int mode);
void msx_host_unload_bios(void);

const uint8_t* msx_host_get_mapped_rom(const char* fileName, unsigned int* size);
const uint8_t* msx_host_get_builtin_file(const char* name, unsigned int* size);

void msx_host_set_model_mode(int mode);
int  msx_host_get_model_mode(void);

void msx_host_set_view_mode(msx_host_view_mode_t mode);
msx_host_view_mode_t msx_host_get_view_mode(void);

int msx_host_prepare_runtime(void);

#ifdef EMU_LOGS_ENABLED
void msx_host_perf_reset(void);
void msx_host_perf_note_frame(int renderedFrame, int palVideo, unsigned int uPeriod);
void msx_host_audio_note_init(unsigned int rate);
void msx_host_audio_note_render(unsigned int requestedSamples, unsigned int playedSamples, unsigned int freeSamplesBefore);
void msx_host_audio_note_write(unsigned int requestedSamples, unsigned int writtenSamples, unsigned int queuedBefore);
#endif

#ifdef __cplusplus
}
#endif
