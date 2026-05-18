#pragma once

/* Central logging gate for both C and C++.
 *
 * EMU_LOGS_ENABLED is the master switch. Without it, generic EMU_LOG calls
 * compile out. Direct C printf calls are always compiled out by default; C code
 * should use EMU_LOG so old core printf noise cannot bypass this layer.
 *
 * Generic/base logs include FPS, heap and core usage.
 * Audio pipeline diagnostics are separated behind AUDIO_LOGS.
 *
 * Per-core/detail flags currently used:
 *   BOOT_DIAG_LOGS
 *   AUDIO_LOGS
 *   MD_LOGS, MD_AUDIO_LOGS, MD_RENDER_LOGS
 *   SNES_LOGS
 *   NES_DIAG_LOGS, NES_BENCHMARK_LOGS
 *   WS_LOGS_ENABLED, WS_BENCHMARK_LOGS, WS_RENDER_PROFILE
 *   WS_CPU_PROFILE, WS_CPU_BRANCH_PROFILE
 *   NGP_TRACE_LOGS
 *   COLECO_DEBUG_LOGS
 *
 * Legacy BENCHMARK_LOGS is kept as a convenience alias for the old global
 * benchmark switch, but the code now uses core-specific benchmark flags.
 */
#include <stdio.h>

#if defined(EMU_LOGS_ENABLED)
#define EMU_LOG_MASTER_ENABLED 1
#else
#define EMU_LOG_MASTER_ENABLED 0
#endif

#if EMU_LOG_MASTER_ENABLED && defined(AUDIO_LOGS)
#define EMU_AUDIO_LOGS_ENABLED 1
#else
#define EMU_AUDIO_LOGS_ENABLED 0
#endif

#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
#define MD_LOGS_ENABLED 1
#else
#define MD_LOGS_ENABLED 0
#endif

#if EMU_LOG_MASTER_ENABLED && (defined(MD_AUDIO_LOGS) || defined(MD_LOGS))
#define MD_AUDIO_LOGS_ENABLED 1
#else
#define MD_AUDIO_LOGS_ENABLED 0
#endif

#if EMU_LOG_MASTER_ENABLED && (defined(MD_RENDER_LOGS) || defined(MD_LOGS))
#define MD_RENDER_LOGS_ENABLED 1
#else
#define MD_RENDER_LOGS_ENABLED 0
#endif

#if EMU_LOG_MASTER_ENABLED && defined(BENCHMARK_LOGS)
#ifndef NES_BENCHMARK_LOGS
#define NES_BENCHMARK_LOGS 1
#endif
#ifndef WS_BENCHMARK_LOGS
#define WS_BENCHMARK_LOGS 1
#endif
#endif

#if !EMU_LOG_MASTER_ENABLED
#undef BOOT_DIAG_LOGS
#undef AUDIO_LOGS
#undef MD_LOGS
#undef MD_AUDIO_LOGS
#undef MD_RENDER_LOGS
#undef BENCHMARK_LOGS
#undef NES_BENCHMARK_LOGS
#undef WS_BENCHMARK_LOGS
#undef WS_RENDER_PROFILE
#undef WS_CPU_PROFILE
#undef WS_CPU_BRANCH_PROFILE
#undef WS_LOGS_ENABLED
#undef COLECO_DEBUG_LOGS
#undef NES_DIAG_LOGS
#undef NGP_TRACE_LOGS
#undef SNES_LOGS
#endif

#if EMU_LOG_MASTER_ENABLED
#define EMU_LOG(...) (printf)(__VA_ARGS__)
#else
#define EMU_LOG(...) do { } while (0)
#endif

#if MD_AUDIO_LOGS_ENABLED
#define MD_AUDIO_LOG(fmt, ...) EMU_LOG("[MD][AUDIO] " fmt "\n", ##__VA_ARGS__)
#else
#define MD_AUDIO_LOG(fmt, ...) ((void)0)
#endif

#if MD_RENDER_LOGS_ENABLED
#define MD_RENDER_LOG(fmt, ...) EMU_LOG("[MD][RENDER] " fmt "\n", ##__VA_ARGS__)
#else
#define MD_RENDER_LOG(fmt, ...) ((void)0)
#endif

#if !defined(__cplusplus) && !defined(EMU_ALLOW_RAW_PRINTF)
#ifdef printf
#undef printf
#endif
#define printf(...) ((void)0)
#endif
