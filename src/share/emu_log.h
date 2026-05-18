#pragma once
/* Global log gating for C and C++ code.
 * EMU_LOGS_ENABLED is the master switch. Per-core/detail flags are inert unless
 * this switch is enabled, so printf-style diagnostics do not sneak into release
 * builds just because a detail flag was left on. */
#include <stdio.h>

#if !defined(EMU_LOGS_ENABLED)
  #ifdef BENCHMARK_LOGS
    #undef BENCHMARK_LOGS
  #endif
  #ifdef WS_LOGS_ENABLED
    #undef WS_LOGS_ENABLED
  #endif
  #ifdef WS_RENDER_PROFILE
    #undef WS_RENDER_PROFILE
  #endif
  #ifdef WS_CPU_PROFILE
    #undef WS_CPU_PROFILE
  #endif
  #ifdef WS_CPU_BRANCH_PROFILE
    #undef WS_CPU_BRANCH_PROFILE
  #endif
  #ifdef NES_DIAG_LOGS
    #undef NES_DIAG_LOGS
  #endif
  #ifdef COLECO_DEBUG_LOGS
    #undef COLECO_DEBUG_LOGS
  #endif
  #ifdef NGP_TRACE_LOGS
    #undef NGP_TRACE_LOGS
  #endif
  #ifdef SNES_LOGS
    #undef SNES_LOGS
  #endif
#endif

#if defined(EMU_LOGS_ENABLED)
  #ifdef __cplusplus
    #define EMU_LOG(...) ::printf(__VA_ARGS__)
  #else
    #define EMU_LOG(...) printf(__VA_ARGS__)
  #endif
#else
  #define EMU_LOG(...) ((int)0)
#endif

#if defined(REMOVE_PRINTF) && !defined(EMU_LOGS_ENABLED) && !defined(__cplusplus)
  #ifdef printf
    #undef printf
  #endif
  #define printf(...) ((int)0)
#endif
