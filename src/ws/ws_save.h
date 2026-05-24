#pragma once
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void ws_save_init(const char* romPathOrName);
void ws_save_load(void);
void ws_save_tick(void);
void ws_save_request_flush(void);
void ws_save_force_flush(void);
bool ws_save_has_sram(void);
bool ws_save_uses_sd_backing(void);
bool ws_save_snapshot_sram(uint8_t** outData, size_t* outSize);
bool ws_save_force_flush_buffer(const uint8_t* data, size_t size);
void ws_save_suspend_background(void);
void ws_save_resume_background(void);
void ws_save_shutdown(void);

#ifdef __cplusplus
}
#endif
