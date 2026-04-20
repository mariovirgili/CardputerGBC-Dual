#include "emu_static_pool.h"

#include <esp_heap_caps.h>

static uint8_t* s_emu_static_pool = 0;

uint8_t* emu_static_pool_get(void)
{
    return s_emu_static_pool;
}

bool emu_static_pool_acquire(void)
{
    if (s_emu_static_pool) {
        return true;
    }

    s_emu_static_pool = (uint8_t*)heap_caps_malloc(EMU_STATIC_POOL_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_emu_static_pool) {
        s_emu_static_pool = (uint8_t*)heap_caps_malloc(EMU_STATIC_POOL_SIZE, MALLOC_CAP_8BIT);
    }
    if (!s_emu_static_pool) {
        s_emu_static_pool = (uint8_t*)heap_caps_malloc(EMU_STATIC_POOL_SIZE, MALLOC_CAP_DEFAULT);
    }

    return s_emu_static_pool != 0;
}

void emu_static_pool_release(void)
{
    if (!s_emu_static_pool) {
        return;
    }

    heap_caps_free(s_emu_static_pool);
    s_emu_static_pool = 0;
}
