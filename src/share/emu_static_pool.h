#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU_STATIC_POOL_SIZE (56u * 1024u)

uint8_t* emu_static_pool_get(void);
bool emu_static_pool_acquire(void);
void emu_static_pool_release(void);

#ifdef __cplusplus
}
#endif
