#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "a7800_config.h"

void a7800_video_init(double fps, unsigned baseWidth, unsigned baseHeight, float aspectRatio);
void a7800_video_shutdown(void);
void a7800_video_show_external_info(const char* romTitle);
void a7800_video_toggle_fullscreen(void);
void a7800_video_adjust_zoom(int delta);
void a7800_video_submit_frame(const void* frame,
                              unsigned width,
                              unsigned height,
                              size_t pitch,
                              const uint16_t* palette565,
                              bool indexed,
                              bool isPal);

void a7800_video_set_frame_skip(bool skip);

/* Read-and-reset video submit timing counters (called once per second from run loop). */
void a7800_video_get_and_reset_stats(int64_t* totalUs, uint32_t* count);

extern bool a7800FullScreen;
extern int a7800ZoomPercent;
