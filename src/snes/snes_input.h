#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void snes_input_start(void);

void snes_input_stop(void);

void snes_input_tick(void);

uint32_t snes_input_poll(void);

#ifdef __cplusplus
}
#endif
