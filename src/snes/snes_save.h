#pragma once

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void snes_save_prepare_sram(void);
void snes_save_init(const char* romPathOrName);
void snes_save_load(void);
void snes_save_tick(void);
void snes_save_request_flush(void);
void snes_save_force_flush(void);
bool snes_save_snapshot_sram(uint8_t** outData, size_t* outSize);
bool snes_save_force_flush_buffer(const uint8_t* data, size_t size);
void snes_save_release_sram(void);
void snes_save_shutdown(void);
void snes_save_suspend_background(void);
void snes_save_resume_background(void);
bool snes_save_has_sram(void);

#ifdef __cplusplus
}
#endif
