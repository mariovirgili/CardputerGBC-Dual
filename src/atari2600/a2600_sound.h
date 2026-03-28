#pragma once

#include <stddef.h>
#include <stdint.h>

void a2600_sound_init(int sampleRate);
void a2600_sound_shutdown(void);
void a2600_sound_submit_stereo(const int16_t* samples, size_t frames);
