#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU_STATIC_POOL_SIZE (60u * 1024u)

extern uint8_t g_emu_static_pool[EMU_STATIC_POOL_SIZE];

#ifdef __cplusplus
}
#endif

