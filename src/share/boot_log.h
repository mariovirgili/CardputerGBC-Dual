#pragma once

#include <stdio.h>
#include "esp_timer.h"
#include "share/emu_log.h"

#if EMU_LOG_MASTER_ENABLED && defined(BOOT_DIAG_LOGS)
#define BOOT_LOG(tag, fmt, ...)                                                \
  do {                                                                         \
    (printf)("[%s][%llu ms] " fmt "\n",                                      \
             tag,                                                              \
             (unsigned long long)(esp_timer_get_time() / 1000ULL),             \
             ##__VA_ARGS__);                                                   \
    fflush(stdout);                                                            \
  } while (0)
#else
#define BOOT_LOG(tag, fmt, ...) ((void)0)
#endif

#if EMU_AUDIO_LOGS_ENABLED
#define AUDIO_LOG(tag, fmt, ...)                                               \
  do {                                                                         \
    (printf)("[%s][%llu ms] " fmt "\n",                                      \
             tag,                                                              \
             (unsigned long long)(esp_timer_get_time() / 1000ULL),             \
             ##__VA_ARGS__);                                                   \
    fflush(stdout);                                                            \
  } while (0)
#else
#define AUDIO_LOG(tag, fmt, ...) ((void)0)
#endif
