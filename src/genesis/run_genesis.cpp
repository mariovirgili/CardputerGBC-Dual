#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "esp_heap_caps.h"
#include "genesis/run_genesis.h"
#include "genesis_sound.h"
#include "genesis_display.h"
#include "genesis_save.h"
#include "genesis/gwenesis/bus/gwenesis_bus.h"
#include "share/input.h"
#include "share/game_save.h"
#include "share/sd_control.h"
#include "share/sd_gameplay_guard.h"
#include "share/utils.h"
#include "share/emu_log_cpp.h"
#include "compat/arduino_compat.h"
#include <M5Cardputer.h>

extern "C" {
  #include "genesis/gwenesis/cpus/M68K/m68k.h"
  #include "genesis/gwenesis/cpus/Z80/Z80.h"
}

#if MD_XTENSA_PERF_COUNTERS
#include "xtensa_perfmon_access.h"
#include "xtensa/xt_perf_consts.h"
#endif

#if EMU_LOG_MASTER_ENABLED
static uint32_t frame_count = 0;
static uint64_t last_fps_log_time = 0;
#endif
static volatile int g_target_fps = 60;
extern int genesisZoomPercent;

#ifndef MD_GEOSKIP_FRAMESKIP
#define MD_GEOSKIP_FRAMESKIP 0
#endif

enum MdFrameskipMode : uint8_t {
  MD_FRAMESKIP_OFF = 0,
  MD_FRAMESKIP_ADAPTIVE,
  MD_FRAMESKIP_FIXED,
#if MD_GEOSKIP_FRAMESKIP
  MD_FRAMESKIP_GEOSKIP,
#endif
};

enum MdMenuPage : uint8_t {
  MD_MENU_MAIN = 0,
  MD_MENU_AUDIO,
  MD_MENU_VIDEO,
  MD_MENU_FPS,
  MD_MENU_HELP,
};

enum MdFpsOverlayMode : uint8_t {
  MD_FPS_OFF = 0,
  MD_FPS_CORE,
  MD_FPS_VIDEO,
};

static bool s_mdMenuOpen = false;
static int s_mdMenuSelected = 0;
static MdMenuPage s_mdMenuPage = MD_MENU_MAIN;
static MdMenuPage s_mdHelpSourcePage = MD_MENU_MAIN;
static int s_mdHelpPageIndex = 0;
static MdFpsOverlayMode s_mdFpsOverlayMode = MD_FPS_OFF;
static MdFrameskipMode s_mdFrameskipMode = MD_FRAMESKIP_OFF;
static bool s_mdWallclockSamples = false;
static bool s_mdPcmSync = true;
struct MdMenuStackEntry {
  MdMenuPage page;
  int selected;
};
static MdMenuStackEntry s_mdMenuStack[8] = {};
static int s_mdMenuStackDepth = 0;
static int s_mdFixedFrameskipIndex = 0;
static uint16_t s_mdFixedSkipCreditQ8 = 0;
static bool s_mdAdaptiveSkipNextDraw = false;
#if MD_GEOSKIP_FRAMESKIP
static uint8_t s_mdGeoSkipFramesToSkip = 0;
#endif
static float s_mdCoreFps = 0.0f;
static float s_mdVideoFps = 0.0f;
static uint32_t s_mdRuntimeCoreFrames = 0;
static uint32_t s_mdRuntimeVideoFrames = 0;
static uint64_t s_mdRuntimeFpsLastUs = 0;
static char s_mdOptionsPath[PATH_MAX] = {0};

static void md_menu_stack_clear();

#ifndef MD_BENCH_NO_FRAME_SKIP
#define MD_BENCH_NO_FRAME_SKIP 0
#endif

#ifndef MD_DETERMINISTIC_BENCH
#define MD_DETERMINISTIC_BENCH 0
#endif

#ifndef MD_DETERMINISTIC_BENCH_EXIT_ON_LIMIT
#define MD_DETERMINISTIC_BENCH_EXIT_ON_LIMIT 0
#endif

#ifndef MD_DETERMINISTIC_BENCH_FRAMES
#define MD_DETERMINISTIC_BENCH_FRAMES 0
#endif

#ifndef MD_DETERMINISTIC_BENCH_START_AT_FRAME
#define MD_DETERMINISTIC_BENCH_START_AT_FRAME 0
#endif

#ifndef MD_DETERMINISTIC_BENCH_START_HOLD_FRAMES
#define MD_DETERMINISTIC_BENCH_START_HOLD_FRAMES 0
#endif

#ifndef MD_M68K_NO_HINT_BATCH_RUN
#define MD_M68K_NO_HINT_BATCH_RUN 0
#endif

#ifndef MD_M68K_NO_HINT_BATCH_LINES
#define MD_M68K_NO_HINT_BATCH_LINES 8
#endif

#ifndef MD_Z80_PREFIX_PROFILING
#define MD_Z80_PREFIX_PROFILING 0
#endif

#ifndef MD_M68K_CATEGORY_PROFILING_DUMP
#define MD_M68K_CATEGORY_PROFILING_DUMP 0
#endif

#ifndef MD_VDP_STATUS_POLL_DIAG
#define MD_VDP_STATUS_POLL_DIAG 0
#endif

#if MD_VDP_STATUS_POLL_DIAG
extern "C" void gwenesis_vdp_status_poll_diag_log_and_reset(void);
#endif

#ifndef MD_XTENSA_PERF_COUNTERS
#define MD_XTENSA_PERF_COUNTERS 0
#endif

#if MD_DETERMINISTIC_BENCH
static uint32_t s_mdDeterministicBenchFrame = 0;
static uint8_t s_mdDeterministicBenchLimitLogged = 0;

extern "C" uint32_t md_deterministic_bench_frame(void)
{
  return s_mdDeterministicBenchFrame;
}
#endif

static inline void md_xtensa_perf_log_and_rotate();

#if MD_RENDER_LOGS_ENABLED
struct MdFrameDiagStats {
  uint64_t lastLogMs = 0;
  uint32_t frames = 0;
  uint32_t drawFrames = 0;
  uint32_t noDrawFrames = 0;
  uint32_t z80Skipped = 0;
  uint32_t lateSkipRequested = 0;
  uint32_t beginSendFail = 0;
  uint32_t endSendFail = 0;
  uint32_t renderedLinesMin = UINT32_MAX;
  uint32_t renderedLinesMax = 0;
  uint64_t renderedLinesTotal = 0;
  uint32_t frameUsMin = UINT32_MAX;
  uint32_t frameUsMax = 0;
  uint64_t frameUsTotal = 0;
  uint32_t overBudget = 0;
  uint32_t hLast = 0;
  uint32_t linesLast = 0;
};

static MdFrameDiagStats s_mdFrameDiag;

static inline uint64_t md_render_now_ms()
{
  return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static inline void md_render_diag_reset()
{
  s_mdFrameDiag = MdFrameDiagStats{};
  s_mdFrameDiag.lastLogMs = md_render_now_ms();
}

static inline void md_render_diag_record(bool drawFrame,
                                         bool skipZ80,
                                         bool lateSkip,
                                         uint32_t renderedLines,
                                         uint32_t frameUs,
                                         uint32_t budgetUs,
                                         uint32_t h,
                                         uint32_t linesPerFrame)
{
  MdFrameDiagStats& d = s_mdFrameDiag;
  ++d.frames;
  if (drawFrame) ++d.drawFrames; else ++d.noDrawFrames;
  if (skipZ80) ++d.z80Skipped;
  if (lateSkip) ++d.lateSkipRequested;
  if (renderedLines < d.renderedLinesMin) d.renderedLinesMin = renderedLines;
  if (renderedLines > d.renderedLinesMax) d.renderedLinesMax = renderedLines;
  d.renderedLinesTotal += renderedLines;
  if (frameUs < d.frameUsMin) d.frameUsMin = frameUs;
  if (frameUs > d.frameUsMax) d.frameUsMax = frameUs;
  d.frameUsTotal += frameUs;
  if (frameUs > budgetUs) ++d.overBudget;
  d.hLast = h;
  d.linesLast = linesPerFrame;
}

static inline void md_render_diag_log_if_due(bool force = false)
{
  MdFrameDiagStats& d = s_mdFrameDiag;
  const uint64_t now = md_render_now_ms();
  if (!force && (now - d.lastLogMs) < 1000ULL) return;
  if (d.frames == 0) {
    d.lastLogMs = now;
    return;
  }

  const uint32_t renderedMin = (d.renderedLinesMin == UINT32_MAX) ? 0 : d.renderedLinesMin;
  const uint32_t frameMin = (d.frameUsMin == UINT32_MAX) ? 0 : d.frameUsMin;
  const uint32_t renderedAvg = (uint32_t)(d.renderedLinesTotal / d.frames);
  const uint32_t frameAvg = (uint32_t)(d.frameUsTotal / d.frames);
  MD_RENDER_LOG("frames=%lu draw=%lu nodraw=%lu z80Skip=%lu lateSkip=%lu overBudget=%lu frameUs min/avg/max=%lu/%lu/%lu renderedLines min/avg/max=%lu/%lu/%lu beginFail=%lu endFail=%lu srcH=%lu lines=%lu zoom=%d field=%d",
                (unsigned long)d.frames,
                (unsigned long)d.drawFrames,
                (unsigned long)d.noDrawFrames,
                (unsigned long)d.z80Skipped,
                (unsigned long)d.lateSkipRequested,
                (unsigned long)d.overBudget,
                (unsigned long)frameMin,
                (unsigned long)frameAvg,
                (unsigned long)d.frameUsMax,
                (unsigned long)renderedMin,
                (unsigned long)renderedAvg,
                (unsigned long)d.renderedLinesMax,
                (unsigned long)d.beginSendFail,
                (unsigned long)d.endSendFail,
                (unsigned long)d.hLast,
                (unsigned long)d.linesLast,
                genesisZoomPercent,
                g_field_ofs);
  d = MdFrameDiagStats{};
  d.lastLogMs = now;
}
#else
static inline void md_render_diag_reset() {}
static inline void md_render_diag_record(bool, bool, bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
static inline void md_render_diag_log_if_due(bool = false) {}
#endif

#if MD_BENCHMARK_LOGS_ENABLED
struct MdBenchStats {
  uint64_t lastLogUs = 0;
  uint32_t frames = 0;
  uint32_t drawFrames = 0;
  uint32_t noDrawFrames = 0;
  uint32_t lateFrames = 0;
  uint32_t overBudget = 0;
  uint32_t z80SkippedFrames = 0;
  uint32_t beginSendFail = 0;
  uint32_t endSendFail = 0;
  uint32_t renderedLinesMax = 0;
  uint64_t renderedLinesTotal = 0;
  uint32_t frameUsMin = UINT32_MAX;
  uint32_t frameUsMax = 0;
  uint64_t frameUsTotal = 0;
  uint64_t m68kUsTotal = 0;
  uint64_t z80UsTotal = 0;
  uint64_t psgUsTotal = 0;
  uint64_t vdpConfigUsTotal = 0;
  uint64_t renderUsTotal = 0;
  uint64_t audioSubmitUsTotal = 0;
};

static MdBenchStats s_mdBench;

static inline uint64_t md_bench_now_us()
{
  return (uint64_t)esp_timer_get_time();
}

static inline void md_bench_reset()
{
  s_mdBench = MdBenchStats{};
  s_mdBench.lastLogUs = md_bench_now_us();
}

static inline uint32_t md_bench_delta_us(uint64_t startUs)
{
  return (uint32_t)(md_bench_now_us() - startUs);
}

static inline void md_bench_add_u64(uint64_t& dst, uint64_t startUs)
{
  dst += md_bench_delta_us(startUs);
}

static inline void md_bench_record_frame(bool drawFrame,
                                         bool skipZ80,
                                         bool lateSkip,
                                         uint32_t renderedLines,
                                         uint32_t frameUs,
                                         uint32_t budgetUs)
{
  MdBenchStats& d = s_mdBench;
  ++d.frames;
  if (drawFrame) ++d.drawFrames; else ++d.noDrawFrames;
  if (skipZ80) ++d.z80SkippedFrames;
  if (lateSkip) ++d.lateFrames;
  if (frameUs > budgetUs) ++d.overBudget;
  if (renderedLines > d.renderedLinesMax) d.renderedLinesMax = renderedLines;
  d.renderedLinesTotal += renderedLines;
  if (frameUs < d.frameUsMin) d.frameUsMin = frameUs;
  if (frameUs > d.frameUsMax) d.frameUsMax = frameUs;
  d.frameUsTotal += frameUs;
}

static inline void md_bench_log_if_due(bool force = false)
{
  MdBenchStats& d = s_mdBench;
  const uint64_t nowUs = md_bench_now_us();
  if (!force && (nowUs - d.lastLogUs) < 1000000ULL) return;
  if (d.frames == 0) {
    d.lastLogUs = nowUs;
    return;
  }

  const uint32_t elapsedMs = (uint32_t)((nowUs - d.lastLogUs) / 1000ULL);
  const uint32_t frameMin = (d.frameUsMin == UINT32_MAX) ? 0 : d.frameUsMin;
  const uint32_t frameAvg = (uint32_t)(d.frameUsTotal / d.frames);
  const uint32_t linesAvg = (uint32_t)(d.renderedLinesTotal / d.frames);
  const uint32_t renderLineAvg = d.renderedLinesTotal ? (uint32_t)(d.renderUsTotal / d.renderedLinesTotal) : 0;
  const float fps = elapsedMs ? ((float)d.frames * 1000.0f / (float)elapsedMs) : 0.0f;

#if MD_BUS_PROBE_LOGS_ENABLED
  gwenesis_bus_probe_log_and_reset();
#endif
#if MD_VDP_STATUS_POLL_DIAG
  gwenesis_vdp_status_poll_diag_log_and_reset();
#endif
  MD_BENCH_LOG("fps=%.1f frames=%lu draw/nodraw=%lu/%lu late=%lu over=%lu frameUs min/avg/max=%lu/%lu/%lu cpu68kUs=%lu z80Us=%lu psgUs=%lu vdpCfgUs=%lu renderUs frame/line=%lu/%lu audioSubmitUs=%lu lines avg/max=%lu/%lu qFail b/e=%lu/%lu heap free/largest/min=%lu/%lu/%lu",
               fps,
               (unsigned long)d.frames,
               (unsigned long)d.drawFrames,
               (unsigned long)d.noDrawFrames,
               (unsigned long)d.lateFrames,
               (unsigned long)d.overBudget,
               (unsigned long)frameMin,
               (unsigned long)frameAvg,
               (unsigned long)d.frameUsMax,
               (unsigned long)(d.m68kUsTotal / d.frames),
               (unsigned long)(d.z80UsTotal / d.frames),
               (unsigned long)(d.psgUsTotal / d.frames),
               (unsigned long)(d.vdpConfigUsTotal / d.frames),
               (unsigned long)(d.renderUsTotal / d.frames),
               (unsigned long)renderLineAvg,
               (unsigned long)(d.audioSubmitUsTotal / d.frames),
               (unsigned long)linesAvg,
               (unsigned long)d.renderedLinesMax,
               (unsigned long)d.beginSendFail,
               (unsigned long)d.endSendFail,
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  md_xtensa_perf_log_and_rotate();

  d = MdBenchStats{};
  d.lastLogUs = nowUs;
}
#else
static inline void md_bench_reset() {}
static inline uint64_t md_bench_now_us() { return 0; }
static inline uint32_t md_bench_delta_us(uint64_t) { return 0; }
static inline void md_bench_add_u64(uint64_t&, uint64_t) {}
static inline void md_bench_record_frame(bool, bool, bool, uint32_t, uint32_t, uint32_t) {}
static inline void md_bench_log_if_due(bool = false) {}
#endif

#if MD_XTENSA_PERF_COUNTERS
struct MdXtensaPerfEvent {
  uint16_t select;
  uint16_t mask;
  const char* name;
};

static const MdXtensaPerfEvent s_mdXtensaPerfEvents[] = {
  { XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_CACHE_MISS, "iStallCacheMiss" },
  { XTPERF_CNT_I_MEM, XTPERF_MASK_I_MEM_CACHE_MISSES, "iMemCacheMiss" },
  { XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_CACHE_MISS, "dStallCacheMiss" },
  { XTPERF_CNT_D_LOAD_U1, XTPERF_MASK_D_LOAD_CACHE_MISSES, "dLoadCacheMiss" },
  { XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL, "insn" },
};

struct MdXtensaPerfStats {
  uint32_t eventIndex = 0;
  uint32_t configured = 0;
  uint64_t calls = 0;
  uint64_t cycles = 0;
  uint64_t event = 0;
};

static MdXtensaPerfStats s_mdXtensaPerf;

static inline void md_xtensa_perf_configure_current()
{
  const MdXtensaPerfEvent& e = s_mdXtensaPerfEvents[s_mdXtensaPerf.eventIndex];
  xtensa_perfmon_stop();
  xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, XTPERF_MASK_CYCLES, 0, 15);
  xtensa_perfmon_init(1, e.select, e.mask, 0, 15);
  s_mdXtensaPerf.configured = 1;
}

static inline void md_xtensa_perf_reset()
{
  xtensa_perfmon_stop();
  s_mdXtensaPerf = MdXtensaPerfStats{};
  md_xtensa_perf_configure_current();
}

static inline void md_xtensa_perf_rotate()
{
  xtensa_perfmon_stop();
  s_mdXtensaPerf.eventIndex =
    (s_mdXtensaPerf.eventIndex + 1u) %
    (uint32_t)(sizeof(s_mdXtensaPerfEvents) / sizeof(s_mdXtensaPerfEvents[0]));
  s_mdXtensaPerf.calls = 0;
  s_mdXtensaPerf.cycles = 0;
  s_mdXtensaPerf.event = 0;
  md_xtensa_perf_configure_current();
}

static inline void md_xtensa_perf_m68k_run(unsigned int cpuDeadline)
{
  if (!s_mdXtensaPerf.configured)
  {
    md_xtensa_perf_configure_current();
  }
  xtensa_perfmon_reset(0);
  xtensa_perfmon_reset(1);
  xtensa_perfmon_start();
  m68k_run(cpuDeadline);
  xtensa_perfmon_stop();
  s_mdXtensaPerf.cycles += xtensa_perfmon_value(0);
  s_mdXtensaPerf.event += xtensa_perfmon_value(1);
  s_mdXtensaPerf.calls++;
}

static inline void md_xtensa_perf_log_and_rotate()
{
  const MdXtensaPerfEvent& e = s_mdXtensaPerfEvents[s_mdXtensaPerf.eventIndex];
  const unsigned long pct10 = s_mdXtensaPerf.cycles ?
    (unsigned long)((s_mdXtensaPerf.event * 1000ULL) / s_mdXtensaPerf.cycles) : 0UL;
  EMU_LOG("[MD][XPERF] scope=m68k_run event=%s calls=%llu cycles=%llu value=%llu value/call=%llu pctCycles=%lu.%lu%%\n",
          e.name,
          (unsigned long long)s_mdXtensaPerf.calls,
          (unsigned long long)s_mdXtensaPerf.cycles,
          (unsigned long long)s_mdXtensaPerf.event,
          s_mdXtensaPerf.calls ? (unsigned long long)(s_mdXtensaPerf.event / s_mdXtensaPerf.calls) : 0ULL,
          pct10 / 10UL,
          pct10 % 10UL);
  md_xtensa_perf_rotate();
}
#else
static inline void md_xtensa_perf_reset() {}
static inline void md_xtensa_perf_m68k_run(unsigned int cpuDeadline) { m68k_run(cpuDeadline); }
static inline void md_xtensa_perf_log_and_rotate() {}
#endif

#ifndef MD_OPCODE_PROFILING_DUMP
#define MD_OPCODE_PROFILING_DUMP 0
#endif

#ifndef MD_OPCODE_PROFILING_DUMP_MS
#define MD_OPCODE_PROFILING_DUMP_MS 10000
#endif

#if MD_OPCODE_PROFILING_DUMP
static uint64_t s_mdOpcodeProfileLastLogUs = 0;

static void md_opcode_profile_format_top_nibbles(const uint32_t* counts,
                                              uint64_t total,
                                              char* out,
                                              size_t outSize)
{
  bool used[16] = {};
  size_t pos = 0;
  if (!outSize) return;
  out[0] = '\0';
  for (int rank = 0; rank < 4; ++rank) {
    int bestIdx = -1;
    uint32_t bestCount = 0;
    for (int i = 0; i < 16; ++i) {
      if (!used[i] && counts[i] > bestCount) {
        bestIdx = i;
        bestCount = counts[i];
      }
    }
    if (bestIdx < 0 || bestCount == 0) break;
    used[bestIdx] = true;
    const unsigned long pct = total ? (unsigned long)((bestCount * 100ULL) / total) : 0UL;
    const int wrote = snprintf(out + pos, outSize - pos, "%s%X:%lu%%",
                               pos ? " " : "",
                               bestIdx,
                               pct);
    if (wrote <= 0 || (size_t)wrote >= (outSize - pos)) break;
    pos += (size_t)wrote;
  }
}

static void md_opcode_profile_format_top_bytes(const uint32_t* counts,
                                            uint64_t total,
                                            char* out,
                                            size_t outSize)
{
  bool used[256] = {};
  size_t pos = 0;
  if (!outSize) return;
  out[0] = '\0';
  for (int rank = 0; rank < 12; ++rank) {
    int bestIdx = -1;
    uint32_t bestCount = 0;
    for (int i = 0; i < 256; ++i) {
      if (!used[i] && counts[i] > bestCount) {
        bestIdx = i;
        bestCount = counts[i];
      }
    }
    if (bestIdx < 0 || bestCount == 0) break;
    used[bestIdx] = true;
    const unsigned long permille = total ? (unsigned long)((bestCount * 1000ULL) / total) : 0UL;
    const int wrote = snprintf(out + pos, outSize - pos, "%s%02X:%lu.%lu%%(%lu)",
                               pos ? " " : "",
                               bestIdx,
                               permille / 10UL,
                               permille % 10UL,
                               (unsigned long)bestCount);
    if (wrote <= 0 || (size_t)wrote >= (outSize - pos)) break;
    pos += (size_t)wrote;
  }
}

static void md_opcode_profile_format_top_exact(const m68k_opcode_profile_entry* entries,
                                               uint64_t total,
                                               char* out,
                                               size_t outSize)
{
  size_t pos = 0;
  if (!outSize) return;
  out[0] = '\0';
  for (int i = 0; i < M68K_OPCODE_PROFILE_TOP_COUNT && i < 8; ++i) {
    if (entries[i].count == 0) break;
    const unsigned long pct = total ? (unsigned long)((entries[i].count * 100ULL) / total) : 0UL;
    const int wrote = snprintf(out + pos, outSize - pos, "%s%04X:%lu%%",
                               pos ? " " : "",
                               entries[i].opcode,
                               pct);
    if (wrote <= 0 || (size_t)wrote >= (outSize - pos)) break;
    pos += (size_t)wrote;
  }
}

static inline void md_opcode_profile_reset()
{
  s_mdOpcodeProfileLastLogUs = md_bench_now_us();
  m68k_opcode_profile_reset();
}

static inline void md_opcode_profile_log_if_due(bool force = false)
{
  const uint64_t nowUs = md_bench_now_us();
  if (!force && (nowUs - s_mdOpcodeProfileLastLogUs) < (uint64_t)MD_OPCODE_PROFILING_DUMP_MS * 1000ULL) return;

  m68k_opcode_profile_snapshot snap{};
  const int hasData = m68k_opcode_profile_get_snapshot(&snap, 1);
  s_mdOpcodeProfileLastLogUs = nowUs;
  if (!hasData || snap.total == 0 || snap.sampled_total == 0) return;

  char exactBuf[128];
  char nibbleBuf[64];
  char byteBuf[256];
  md_opcode_profile_format_top_exact(snap.top_exact, snap.sampled_total, exactBuf, sizeof(exactBuf));
  md_opcode_profile_format_top_nibbles(snap.top_nibble, snap.sampled_total, nibbleBuf, sizeof(nibbleBuf));
  md_opcode_profile_format_top_bytes(snap.top_byte, snap.sampled_total, byteBuf, sizeof(byteBuf));
  EMU_LOG("[MD][OPPROF] total=%llu sampled=%llu stride=1/%lu tracked=%lu overflow=%lu exact=%s nib=%s byte=%s\n",
          (unsigned long long)snap.total,
          (unsigned long long)snap.sampled_total,
          (unsigned long)(1u << snap.sample_shift),
          (unsigned long)snap.tracked_opcodes,
          (unsigned long)snap.overflow_events,
          exactBuf[0] ? exactBuf : "-",
          nibbleBuf[0] ? nibbleBuf : "-",
          byteBuf[0] ? byteBuf : "-");
}
#else
static inline void md_opcode_profile_reset() {}
static inline void md_opcode_profile_log_if_due(bool = false) {}
#endif

#if MD_M68K_CATEGORY_PROFILING_DUMP
static uint64_t s_mdM68kCategoryProfileLastLogUs = 0;

static void md_m68k_category_format_top_bytes(const uint32_t* counts,
                                              uint64_t total,
                                              char* out,
                                              size_t outSize)
{
  bool used[256] = {};
  size_t pos = 0;
  if (!outSize) return;
  out[0] = '\0';
  for (int rank = 0; rank < 8; ++rank) {
    int bestIdx = -1;
    uint32_t bestCount = 0;
    for (int i = 0; i < 256; ++i) {
      if (!used[i] && counts[i] > bestCount) {
        bestIdx = i;
        bestCount = counts[i];
      }
    }
    if (bestIdx < 0 || bestCount == 0) break;
    used[bestIdx] = true;
    const unsigned long permille = total ? (unsigned long)((bestCount * 1000ULL) / total) : 0UL;
    const int wrote = snprintf(out + pos, outSize - pos, "%s%02X:%lu.%lu%%",
                               pos ? " " : "",
                               bestIdx,
                               permille / 10UL,
                               permille % 10UL);
    if (wrote <= 0 || (size_t)wrote >= (outSize - pos)) break;
    pos += (size_t)wrote;
  }
}

static void md_m68k_category_format_top_nibbles(const uint32_t* counts,
                                                uint64_t total,
                                                char* out,
                                                size_t outSize)
{
  bool used[16] = {};
  size_t pos = 0;
  if (!outSize) return;
  out[0] = '\0';
  for (int rank = 0; rank < 6; ++rank) {
    int bestIdx = -1;
    uint32_t bestCount = 0;
    for (int i = 0; i < 16; ++i) {
      if (!used[i] && counts[i] > bestCount) {
        bestIdx = i;
        bestCount = counts[i];
      }
    }
    if (bestIdx < 0 || bestCount == 0) break;
    used[bestIdx] = true;
    const unsigned long permille = total ? (unsigned long)((bestCount * 1000ULL) / total) : 0UL;
    const int wrote = snprintf(out + pos, outSize - pos, "%s%X:%lu.%lu%%",
                               pos ? " " : "",
                               bestIdx,
                               permille / 10UL,
                               permille % 10UL);
    if (wrote <= 0 || (size_t)wrote >= (outSize - pos)) break;
    pos += (size_t)wrote;
  }
}

static inline unsigned long long md_m68k_category_avg(uint64_t value, uint64_t count)
{
  return count ? (unsigned long long)(value / count) : 0ULL;
}

static inline uint64_t md_m68k_category_sub_or_zero(uint64_t value, uint64_t sub)
{
  return (value > sub) ? (value - sub) : 0ULL;
}

static inline void md_m68k_category_profile_reset()
{
  s_mdM68kCategoryProfileLastLogUs = (uint64_t)esp_timer_get_time();
  m68k_category_profile_reset();
}

static inline void md_m68k_category_profile_log_if_due(bool force = false)
{
  const uint64_t nowUs = (uint64_t)esp_timer_get_time();
  if (!force && (nowUs - s_mdM68kCategoryProfileLastLogUs) < (uint64_t)MD_OPCODE_PROFILING_DUMP_MS * 1000ULL) return;

  m68k_category_profile_snapshot snap{};
  const int hasData = m68k_category_profile_get_snapshot(&snap, 1);
  s_mdM68kCategoryProfileLastLogUs = nowUs;
  if (!hasData || snap.instructions == 0 || snap.sampled_instructions == 0) return;

  char nibbleBuf[64];
  char byteBuf[160];
  md_m68k_category_format_top_nibbles(snap.top_nibble, snap.sampled_instructions, nibbleBuf, sizeof(nibbleBuf));
  md_m68k_category_format_top_bytes(snap.top_byte, snap.sampled_instructions, byteBuf, sizeof(byteBuf));

  const uint64_t handlerSub =
    snap.handler_imm_cycles +
    snap.handler_read_cycles +
    snap.handler_write_cycles +
    snap.ea_cycles;
  const uint64_t handlerExclusive = md_m68k_category_sub_or_zero(snap.handler_cycles, handlerSub);

  EMU_LOG("[MD][68KCAT] instr=%llu sampled=%llu stride=1/%lu run calls=%llu early=%llu stopped=%llu cyc/call total=%llu loop=%llu cyc/sample handler=%llu ex=%llu ea=%llu himm=%llu hrd=%llu hwr=%llu imm=%llu rd=%llu wr=%llu pc rom/ram=%lu/%lu nib=%s byte=%s\n",
          (unsigned long long)snap.instructions,
          (unsigned long long)snap.sampled_instructions,
          (unsigned long)(1u << snap.sample_shift),
          (unsigned long long)snap.run_calls,
          (unsigned long long)snap.run_early_returns,
          (unsigned long long)snap.run_stopped_returns,
          md_m68k_category_avg(snap.run_total_cycles, snap.run_calls),
          md_m68k_category_avg(snap.run_loop_cycles, snap.run_calls),
          md_m68k_category_avg(snap.handler_cycles, snap.sampled_instructions),
          md_m68k_category_avg(handlerExclusive, snap.sampled_instructions),
          md_m68k_category_avg(snap.ea_cycles, snap.sampled_instructions),
          md_m68k_category_avg(snap.handler_imm_cycles, snap.sampled_instructions),
          md_m68k_category_avg(snap.handler_read_cycles, snap.sampled_instructions),
          md_m68k_category_avg(snap.handler_write_cycles, snap.sampled_instructions),
          md_m68k_category_avg(snap.imm_cycles, snap.sampled_instructions),
          md_m68k_category_avg(snap.data_read_cycles, snap.sampled_instructions),
          md_m68k_category_avg(snap.data_write_cycles, snap.sampled_instructions),
          (unsigned long)snap.pc_rom_count,
          (unsigned long)snap.pc_ram_count,
          nibbleBuf[0] ? nibbleBuf : "-",
          byteBuf[0] ? byteBuf : "-");
  EMU_LOG("[MD][68KMEM] imm16/32=%lu/%lu rd rom/ram/sram/bus=%lu/%lu/%lu/%lu rd8/16/32=%lu/%lu/%lu wr ram/sram/bus=%lu/%lu/%lu wr8/16/32=%lu/%lu/%lu\n",
          (unsigned long)snap.imm16_count,
          (unsigned long)snap.imm32_count,
          (unsigned long)snap.read_rom_count,
          (unsigned long)snap.read_ram_count,
          (unsigned long)snap.read_sram_count,
          (unsigned long)snap.read_bus_count,
          (unsigned long)snap.read8_count,
          (unsigned long)snap.read16_count,
          (unsigned long)snap.read32_count,
          (unsigned long)snap.write_ram_count,
          (unsigned long)snap.write_sram_count,
          (unsigned long)snap.write_bus_count,
          (unsigned long)snap.write8_count,
          (unsigned long)snap.write16_count,
          (unsigned long)snap.write32_count);
  EMU_LOG("[MD][68KEA] di=%lu ix=%lu aw=%lu al=%lu pcdi=%lu pcix=%lu\n",
          (unsigned long)snap.ea_di_count,
          (unsigned long)snap.ea_ix_count,
          (unsigned long)snap.ea_aw_count,
          (unsigned long)snap.ea_al_count,
          (unsigned long)snap.ea_pcdi_count,
          (unsigned long)snap.ea_pcix_count);
}
#else
static inline void md_m68k_category_profile_reset() {}
static inline void md_m68k_category_profile_log_if_due(bool = false) {}
#endif

#if MD_Z80_PREFIX_PROFILING
static uint64_t s_mdZ80PrefixProfileLastLogUs = 0;

static inline void md_z80_prefix_profile_reset()
{
  s_mdZ80PrefixProfileLastLogUs = md_bench_now_us();
  z80_prefix_profile_reset();
}

static inline unsigned long md_z80_prefix_permille(unsigned long long part,
                                                   unsigned long long total)
{
  return total ? (unsigned long)((part * 1000ULL) / total) : 0UL;
}

static inline void md_z80_prefix_profile_log_if_due(bool force = false)
{
  const uint64_t nowUs = md_bench_now_us();
  if (!force && (nowUs - s_mdZ80PrefixProfileLastLogUs) < (uint64_t)MD_OPCODE_PROFILING_DUMP_MS * 1000ULL) return;

  Z80PrefixProfile snap{};
  const int hasData = z80_prefix_profile_get_snapshot(&snap, 1);
  s_mdZ80PrefixProfileLastLogUs = nowUs;
  if (!hasData || snap.opcodes == 0) return;

  const unsigned long cb = md_z80_prefix_permille(snap.cb, snap.opcodes);
  const unsigned long ed = md_z80_prefix_permille(snap.ed, snap.opcodes);
  const unsigned long dd = md_z80_prefix_permille(snap.dd, snap.opcodes);
  const unsigned long fd = md_z80_prefix_permille(snap.fd, snap.opcodes);
  const unsigned long ddcb = md_z80_prefix_permille(snap.ddcb, snap.opcodes);
  const unsigned long fdcb = md_z80_prefix_permille(snap.fdcb, snap.opcodes);
  EMU_LOG("[MD][Z80PROF] ops=%llu cb=%llu:%lu.%lu%% ed=%llu:%lu.%lu%% dd=%llu:%lu.%lu%% fd=%llu:%lu.%lu%% ddcb=%llu:%lu.%lu%% fdcb=%llu:%lu.%lu%%\n",
          snap.opcodes,
          snap.cb, cb / 10UL, cb % 10UL,
          snap.ed, ed / 10UL, ed % 10UL,
          snap.dd, dd / 10UL, dd % 10UL,
          snap.fd, fd / 10UL, fd % 10UL,
          snap.ddcb, ddcb / 10UL, ddcb % 10UL,
          snap.fdcb, fdcb / 10UL, fdcb % 10UL);
}
#else
static inline void md_z80_prefix_profile_reset() {}
static inline void md_z80_prefix_profile_log_if_due(bool = false) {}
#endif

extern "C" {
  #include "genesis/gwenesis/vdp/gwenesis_vdp.h"
  #include "genesis/gwenesis/cpus/M68K/m68k.h"
  #include "genesis/gwenesis/sound/z80inst.h"
  #include "genesis/gwenesis/bus/gwenesis_bus.h"

  extern unsigned char  *M68K_RAM;
  extern uint8_t  *ZRAM;
  extern uint8_t  *VRAM;
  extern uint8_t  gwenesis_vdp_regs[];
  extern unsigned short gwenesis_vdp_status;
  extern int hint_pending;

  int scan_line = 0;
  extern unsigned int screen_width;
  extern unsigned int screen_height;

  // API Gwenesis
  void           load_cartridge(unsigned char *buffer, size_t size);
  void           power_on(void);
  void           reset_emulation(void);
  void           gwenesis_io_pad_release_button(int pad, int idx);

#ifndef GENESIS_NO_SOUND
  void           gwenesis_SN76489_run(int target);
  void           gwenesis_SN76489_Init(int PSGClockValue, int SamplingRate, int freq_divisor);
  void           gwenesis_vdp_allocate_buffers();
#endif
}

/* Allocate memory if pointer is null, abort on failure */
static inline void ensure_alloc(void** p, size_t bytes, const char* name) {
  if (!*p) {
    *p = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!*p) { EMU_LOG("[FATAL] alloc %s (%u bytes) failed\n", name, (unsigned)bytes); abort(); }
  }
}

/* Allocate buffers needed before cartridge header/SRAM probing. */
static void genesis_alloc_cpu_buffers(void) {
  ensure_alloc((void**)&M68K_RAM, MAX_RAM_SIZE, "M68K RAM");   // 64K main RAM
  ensure_alloc((void**)&ZRAM, MAX_Z80_RAM_SIZE, "Z80 RAM");    // 8K Z80 RAM
}

/* Allocate VRAM after SRAM has claimed its contiguous block. */
static void genesis_alloc_vram_buffer(void) {
  ensure_alloc((void**)&VRAM, VRAM_MAX_SIZE, "VRAM");          // 64 KiB
}

static void genesis_alloc_vdp_buffers(void) {
  gwenesis_vdp_allocate_buffers();
}

static void md_log_heap_step(const char* step)
{
  EMU_LOG("[MD][HEAP] %-18s free=%lu largest=%lu min=%lu\n",
          step ? step : "",
          (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
          (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

static void genesis_free_core_runtime_buffers(void) {
  gwenesis_vdp_free_buffers();
  if (VRAM) {
    heap_caps_free(VRAM);
    VRAM = nullptr;
  }
  if (M68K_RAM) {
    heap_caps_free(M68K_RAM);
    M68K_RAM = nullptr;
  }
  if (ZRAM) {
    heap_caps_free(ZRAM);
    ZRAM = nullptr;
  }
}

static bool md_has_sram()
{
  return SRAM_ENABLED && SRAM && SRAM_SIZE != 0;
}

static void md_reset_quit_controls()
{
  share::setRestartRequestMode(false);
  share::clearRestartRequest();
  share::clearBeforeRestartCallback();
}

static void md_options_save();

static void md_enter_sd_off_gameplay()
{
#ifdef MD_SD_OFF_DURING_GAMEPLAY
  share::clearRestartRequest();
  share::setRestartRequestMode(true);
  share::clearBeforeRestartCallback();
  genesis_save_suspend_background();
  share_sd_gameplay_close("MD");
#else
  share::setBeforeRestartCallback(genesis_save_force_flush);
#endif
}

static void md_load_sram_with_sd()
{
  if (!md_has_sram()) {
    genesis_save_load();
    return;
  }

  const bool was_mounted = share_sd_is_mounted();
  if (!was_mounted && !share_sd_gameplay_mount("MD", "load")) {
    EMU_LOG("[GEN][SAVE] SD remount failed, SRAM load skipped\n");
    return;
  }

  genesis_save_load();

#ifdef MD_SD_OFF_DURING_GAMEPLAY
  if (!was_mounted) {
    share_sd_gameplay_close_if_mounted("MD", "load");
  }
#endif
}

static void md_clean_teardown_and_save()
{
  EMU_LOG("[MD][QUIT] clean teardown start\n");
  genesis_save_suspend_background();
  md_log_heap_step("quit start");

  genesis_display_stop();
#ifndef GENESIS_NO_SOUND
  genesis_free_audio_buffers();
#endif
  md_log_heap_step("tasks stopped");

  genesis_free_core_runtime_buffers();
  md_log_heap_step("core freed");

  if (md_has_sram() || s_mdOptionsPath[0] != '\0') {
    md_log_heap_step("pre SD begin");
    if (share_sd_gameplay_mount("MD", "save")) {
      md_log_heap_step("after SD begin");
      if (md_has_sram()) {
        genesis_save_force_flush();
      } else {
        EMU_LOG("[MD][SD] no SRAM, save options only\n");
      }
      md_options_save();
      md_log_heap_step("after save");
    } else {
      EMU_LOG("[GEN][SAVE] SD remount failed, SRAM/options not saved\n");
    }
  } else {
    EMU_LOG("[MD][SD] no SRAM/options, skip SD remount/save\n");
  }

#ifdef MD_SD_OFF_DURING_GAMEPLAY
  share_sd_gameplay_close_if_mounted("MD", "save");
#endif

  genesis_save_shutdown();
  md_reset_quit_controls();
  EMU_LOG("[MD][QUIT] clean teardown done\n");
}

static constexpr uint16_t kMdFixedFrameskipQ8[] = {
  51,   // 0.2 skipped frames per drawn frame
  102,  // 0.4
  128,  // 0.5
  205,  // 0.8
  256,  // 1.0
  384,  // 1.5
};

static constexpr const char* kMdFixedFrameskipLabels[] = {
  "Fixed 0.2",
  "Fixed 0.4",
  "Fixed 0.5",
  "Fixed 0.8",
  "Fixed 1.0",
  "Fixed 1.5",
};

static inline void md_release_gamepad_buttons()
{
  for (int i = 0; i < 8; ++i) {
    gwenesis_io_pad_release_button(0, i);
  }
}

static const char* md_frameskip_label()
{
  if (s_mdFrameskipMode == MD_FRAMESKIP_ADAPTIVE) return "Adaptive";
#if MD_GEOSKIP_FRAMESKIP
  if (s_mdFrameskipMode == MD_FRAMESKIP_GEOSKIP) return "GeoSkip";
#endif
  if (s_mdFrameskipMode == MD_FRAMESKIP_FIXED) return kMdFixedFrameskipLabels[s_mdFixedFrameskipIndex];
  return "Off";
}

static const char* md_fps_mode_label()
{
  switch (s_mdFpsOverlayMode) {
    case MD_FPS_CORE: return "Core";
    case MD_FPS_VIDEO: return "Video";
    case MD_FPS_OFF:
    default:
      return "Off";
  }
}

static const char* md_wallclock_label()
{
  return s_mdWallclockSamples ? "On" : "Off";
}

static const char* md_pcm_sync_label()
{
  return s_mdPcmSync ? "On" : "Off";
}

static float md_fps_overlay_value()
{
  switch (s_mdFpsOverlayMode) {
    case MD_FPS_CORE: return s_mdCoreFps;
    case MD_FPS_VIDEO: return s_mdVideoFps;
    case MD_FPS_OFF:
    default:
      return 0.0f;
  }
}

static void md_apply_fps_overlay()
{
  genesis_display_set_fps_overlay(s_mdFpsOverlayMode != MD_FPS_OFF, md_fps_overlay_value());
}

static void md_set_fps_mode(MdFpsOverlayMode mode)
{
  if (mode > MD_FPS_VIDEO) mode = MD_FPS_OFF;
  s_mdFpsOverlayMode = mode;
  md_apply_fps_overlay();
}

static void md_cycle_fps_mode(int dir)
{
  int slot = (int)s_mdFpsOverlayMode;
  slot = (slot + dir + 3) % 3;
  md_set_fps_mode((MdFpsOverlayMode)slot);
}

static void md_set_wallclock_samples(bool enabled)
{
  s_mdWallclockSamples = enabled;
  genesis_sound_set_wallclock_samples(enabled);
}

static void md_toggle_wallclock_samples()
{
  md_set_wallclock_samples(!s_mdWallclockSamples);
}

static void md_set_pcm_sync(bool enabled)
{
  s_mdPcmSync = enabled;
  genesis_sound_set_pcm_sync(enabled);
}

static void md_toggle_pcm_sync()
{
  md_set_pcm_sync(!s_mdPcmSync);
}

static int md_help_page_count(MdMenuPage sourcePage)
{
  switch (sourcePage) {
    case MD_MENU_AUDIO:
    case MD_MENU_VIDEO:
      return 2;
    case MD_MENU_MAIN:
    case MD_MENU_FPS:
    default:
      return 1;
  }
}

static void md_cycle_help_page(int dir)
{
  const int count = md_help_page_count(s_mdHelpSourcePage);
  if (count <= 1) {
    s_mdHelpPageIndex = 0;
    return;
  }
  s_mdHelpPageIndex = (s_mdHelpPageIndex + dir + count) % count;
}

static void md_show_help_overlay()
{
  const int pageCount = md_help_page_count(s_mdHelpSourcePage);
  if (s_mdHelpPageIndex < 0 || s_mdHelpPageIndex >= pageCount) {
    s_mdHelpPageIndex = 0;
  }

  char title[24];
  const char* line0 = "";
  const char* line1 = "";
  const char* line2 = "";
  const char* line3 = "";
  const char* line4 = "";
  const char* line5 = "";

  if (s_mdHelpSourcePage == MD_MENU_AUDIO) {
    snprintf(title, sizeof(title), "AUDIO HELP %d/%d", s_mdHelpPageIndex + 1, pageCount);
    if (s_mdHelpPageIndex == 0) {
      line0 = "Sync for PCM:";
      line1 = "On  syncs YM DAC";
      line2 = "Best for samples";
      line3 = "Off faster path";
      line4 = "Saved per ROM";
      line5 = "";
    } else {
      line0 = "WallClk:";
      line1 = "Off fixed samples";
      line2 = "On follows time";
      line3 = "Can help slow core";
      line4 = "May vary chunks";
      line5 = "Saved per ROM";
    }
  } else if (s_mdHelpSourcePage == MD_MENU_VIDEO) {
    snprintf(title, sizeof(title), "VIDEO HELP %d/%d", s_mdHelpPageIndex + 1, pageCount);
    if (s_mdHelpPageIndex == 0) {
      line0 = "FPS:";
      line1 = "Off hides HUD";
      line2 = "Core emu frames";
      line3 = "Video drawn frames";
      line4 = "START opens list";
      line5 = "";
    } else {
      line0 = "Frameskip:";
      line1 = "Off draw all";
      line2 = "Adaptive late skip";
      line3 = "GeoSkip render+Z80";
      line4 = "Fixed skip rates";
      line5 = "Audio keeps running";
    }
  } else if (s_mdHelpSourcePage == MD_MENU_FPS) {
    snprintf(title, sizeof(title), "FPS HELP");
    line0 = "CORE:";
    line1 = "Emulator loop fps";
    line2 = "VIDEO:";
    line3 = "Visible draw fps";
    line4 = "START selects";
    line5 = "DEL returns";
  } else {
    snprintf(title, sizeof(title), "CONFIG HELP");
    line0 = "AUDIO:";
    line1 = "Sound timing opts";
    line2 = "VIDEO:";
    line3 = "FPS and frameskip";
    line4 = "START enters";
    line5 = "GO closes menu";
  }

  genesis_display_set_help_overlay(s_mdMenuOpen,
                                   title,
                                   line0,
                                   line1,
                                   line2,
                                   line3,
                                   line4,
                                   line5,
                                   "H/< > page",
                                   "DEL back GO close");
}

static void md_update_menu_overlay()
{
  if (s_mdMenuPage == MD_MENU_HELP) {
    md_show_help_overlay();
  } else if (s_mdMenuPage == MD_MENU_FPS) {
    genesis_display_set_menu_overlay(s_mdMenuOpen,
                                     s_mdMenuSelected,
                                     "FPS SOURCE",
                                     "CORE",
                                     s_mdFpsOverlayMode == MD_FPS_CORE ? "On" : "",
                                     "VIDEO",
                                     s_mdFpsOverlayMode == MD_FPS_VIDEO ? "On" : "",
                                     "",
                                     "",
                                     "START select",
                                     "H help DEL back");
  } else if (s_mdMenuPage == MD_MENU_MAIN) {
    genesis_display_set_menu_overlay(s_mdMenuOpen,
                                     s_mdMenuSelected,
                                     "CONFIG MENU",
                                     "AUDIO",
                                     "",
                                     "VIDEO",
                                     "",
                                     "",
                                     "",
                                     "START enter",
                                     "H help GO close");
  } else if (s_mdMenuPage == MD_MENU_AUDIO) {
    genesis_display_set_menu_overlay(s_mdMenuOpen,
                                     s_mdMenuSelected,
                                     "AUDIO MENU",
                                     "Sync for PCM",
                                     md_pcm_sync_label(),
                                     "WallClk",
                                     md_wallclock_label(),
                                     "",
                                     "",
                                     "START < > toggle",
                                     "H help DEL back");
  } else {
    genesis_display_set_menu_overlay(s_mdMenuOpen,
                                     s_mdMenuSelected,
                                     "VIDEO MENU",
                                     "FPS",
                                     md_fps_mode_label(),
                                     "FRAMESKIP",
                                     md_frameskip_label(),
                                     "",
                                     "",
                                     "START enter < > chg",
                                     "H help DEL back");
  }
  genesis_display_request_overlay_blocking(50);
}

static void md_set_frameskip_mode(MdFrameskipMode mode, int fixedIndex)
{
  const int fixedCount = (int)(sizeof(kMdFixedFrameskipQ8) / sizeof(kMdFixedFrameskipQ8[0]));
  if (fixedIndex < 0) fixedIndex = fixedCount - 1;
  if (fixedIndex >= fixedCount) fixedIndex = 0;
#if !MD_GEOSKIP_FRAMESKIP
  if (mode > MD_FRAMESKIP_FIXED) mode = MD_FRAMESKIP_OFF;
#endif
  s_mdFrameskipMode = mode;
  s_mdFixedFrameskipIndex = fixedIndex;
  s_mdFixedSkipCreditQ8 = 0;
  s_mdAdaptiveSkipNextDraw = false;
#if MD_GEOSKIP_FRAMESKIP
  s_mdGeoSkipFramesToSkip = 0;
#endif
}

static void md_cycle_frameskip(int dir)
{
  const int fixedCount = (int)(sizeof(kMdFixedFrameskipQ8) / sizeof(kMdFixedFrameskipQ8[0]));
  int slot = 0;
  if (s_mdFrameskipMode == MD_FRAMESKIP_ADAPTIVE) {
    slot = 1;
#if MD_GEOSKIP_FRAMESKIP
  } else if (s_mdFrameskipMode == MD_FRAMESKIP_GEOSKIP) {
    slot = 2;
#endif
  } else if (s_mdFrameskipMode == MD_FRAMESKIP_FIXED) {
#if MD_GEOSKIP_FRAMESKIP
    slot = 3 + s_mdFixedFrameskipIndex;
#else
    slot = 2 + s_mdFixedFrameskipIndex;
#endif
  }

  const int slotCount = 2 + fixedCount + (MD_GEOSKIP_FRAMESKIP ? 1 : 0);
  slot = (slot + dir + slotCount) % slotCount;
  if (slot == 0) {
    md_set_frameskip_mode(MD_FRAMESKIP_OFF, 0);
  } else if (slot == 1) {
    md_set_frameskip_mode(MD_FRAMESKIP_ADAPTIVE, 0);
#if MD_GEOSKIP_FRAMESKIP
  } else if (slot == 2) {
    md_set_frameskip_mode(MD_FRAMESKIP_GEOSKIP, 0);
#endif
  } else {
    md_set_frameskip_mode(MD_FRAMESKIP_FIXED, slot - 2 - (MD_GEOSKIP_FRAMESKIP ? 1 : 0));
  }
}

static void md_runtime_options_reset()
{
  s_mdMenuOpen = false;
  s_mdMenuSelected = 0;
  s_mdMenuPage = MD_MENU_MAIN;
  md_menu_stack_clear();
  md_set_fps_mode(MD_FPS_CORE);
  md_set_wallclock_samples(false);
  md_set_pcm_sync(true);
  md_set_frameskip_mode(MD_FRAMESKIP_ADAPTIVE, 0);
  s_mdCoreFps = 0.0f;
  s_mdVideoFps = 0.0f;
  s_mdRuntimeCoreFrames = 0;
  s_mdRuntimeVideoFrames = 0;
  s_mdRuntimeFpsLastUs = 0;
}

static void md_options_make_path(const char* romPathOrName)
{
  s_mdOptionsPath[0] = '\0';

  const char* base = share::gameSaveBasename(romPathOrName);
  char name[160] = {0};
  if (base && *base) {
    strncpy(name, base, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char* dot = strrchr(name, '.');
    if (dot) {
      *dot = '\0';
    }
    strncat(name, ".opt", sizeof(name) - strlen(name) - 1);
  } else {
    strcpy(name, "genesis_autosave.opt");
  }

  const int n = snprintf(s_mdOptionsPath,
                         sizeof(s_mdOptionsPath),
                         "/sd/genesis_saves/%s",
                         name);
  if (n < 0 || (size_t)n >= sizeof(s_mdOptionsPath)) {
    s_mdOptionsPath[sizeof(s_mdOptionsPath) - 1] = '\0';
  }
}

static void md_options_load()
{
  if (s_mdOptionsPath[0] == '\0') return;
  if (!share::gameSaveEnsureParentReady("/sd/genesis_saves")) {
    EMU_LOG("[MD][OPT] storage path not ready, use defaults\n");
    return;
  }

  FILE* f = fopen(s_mdOptionsPath, "rb");
  if (!f) {
    EMU_LOG("[MD][OPT] no existing options for %s\n", s_mdOptionsPath);
    return;
  }

  int showFps = s_mdFpsOverlayMode != MD_FPS_OFF ? 1 : 0;
  int fpsMode = (int)s_mdFpsOverlayMode;
  bool hasFpsMode = false;
  int mode = (int)s_mdFrameskipMode;
  int fixedIndex = s_mdFixedFrameskipIndex;
  int wallclk = s_mdWallclockSamples ? 1 : 0;
  int pcmSync = s_mdPcmSync ? 1 : 0;
  char line[96];
  while (fgets(line, sizeof(line), f)) {
    int value = 0;
    if (sscanf(line, "fps=%d", &value) == 1) {
      showFps = value ? 1 : 0;
    } else if (sscanf(line, "fps_mode=%d", &value) == 1) {
      fpsMode = value;
      hasFpsMode = true;
    } else if (sscanf(line, "frameskip_mode=%d", &value) == 1) {
      mode = value;
    } else if (sscanf(line, "fixed_index=%d", &value) == 1) {
      fixedIndex = value;
    } else if (sscanf(line, "wallclk=%d", &value) == 1) {
      wallclk = value ? 1 : 0;
    } else if (sscanf(line, "pcm_sync=%d", &value) == 1) {
      pcmSync = value ? 1 : 0;
    }
  }
  fclose(f);

  const int maxFrameskipMode =
#if MD_GEOSKIP_FRAMESKIP
      (int)MD_FRAMESKIP_GEOSKIP;
#else
      (int)MD_FRAMESKIP_FIXED;
#endif
  if (mode < (int)MD_FRAMESKIP_OFF || mode > maxFrameskipMode) {
    mode = (int)MD_FRAMESKIP_OFF;
  }
  if (fpsMode < (int)MD_FPS_OFF || fpsMode > (int)MD_FPS_VIDEO) {
    fpsMode = (int)MD_FPS_OFF;
  }
  if (!hasFpsMode) {
    fpsMode = showFps ? (int)MD_FPS_CORE : (int)MD_FPS_OFF;
  }
  md_set_fps_mode((MdFpsOverlayMode)fpsMode);
  md_set_frameskip_mode((MdFrameskipMode)mode, fixedIndex);
  md_set_wallclock_samples(wallclk != 0);
  md_set_pcm_sync(pcmSync != 0);
  EMU_LOG("[MD][OPT] loaded %s fps=%s frameskip=%s wallclk=%s pcmSync=%s\n",
          s_mdOptionsPath,
          md_fps_mode_label(),
          md_frameskip_label(),
          md_wallclock_label(),
          md_pcm_sync_label());
}

static void md_options_load_with_sd()
{
  const bool wasMounted = share_sd_is_mounted();
  if (!wasMounted && !share_sd_gameplay_mount("MD", "options load")) {
    EMU_LOG("[MD][OPT] SD remount failed, use defaults\n");
    return;
  }

  md_options_load();

#ifdef MD_SD_OFF_DURING_GAMEPLAY
  if (!wasMounted) {
    share_sd_gameplay_close_if_mounted("MD", "options load");
  }
#endif
}

static void md_options_save()
{
  if (s_mdOptionsPath[0] == '\0') return;
  if (!share::gameSaveEnsureParentReady("/sd/genesis_saves")) {
    EMU_LOG("[MD][OPT] storage path not ready, options not saved\n");
    return;
  }

  share::setGameIsSaving(true);
  FILE* f = fopen(s_mdOptionsPath, "wb");
  if (!f) {
    share::setGameIsSaving(false);
    EMU_LOG("[MD][OPT] open failed for %s\n", s_mdOptionsPath);
    return;
  }

  const int fixedIndex = (s_mdFrameskipMode == MD_FRAMESKIP_FIXED) ? s_mdFixedFrameskipIndex : 0;
  const int n = fprintf(f,
                        "version=1\n"
                        "fps=%d\n"
                        "fps_mode=%d\n"
                        "frameskip_mode=%d\n"
                        "fixed_index=%d\n"
                        "wallclk=%d\n"
                        "pcm_sync=%d\n",
                        s_mdFpsOverlayMode != MD_FPS_OFF ? 1 : 0,
                        (int)s_mdFpsOverlayMode,
                        (int)s_mdFrameskipMode,
                        fixedIndex,
                        s_mdWallclockSamples ? 1 : 0,
                        s_mdPcmSync ? 1 : 0);
  const int closeOk = fclose(f);
  share::setGameIsSaving(false);

  if (n < 0 || closeOk != 0) {
    EMU_LOG("[MD][OPT] write failed for %s\n", s_mdOptionsPath);
    return;
  }
  EMU_LOG("[MD][OPT] saved %s fps=%s frameskip=%s wallclk=%s pcmSync=%s\n",
          s_mdOptionsPath,
          md_fps_mode_label(),
          md_frameskip_label(),
          md_wallclock_label(),
          md_pcm_sync_label());
}

static bool md_frameskip_should_draw()
{
  if (s_mdMenuOpen) return false;
  if (s_mdFrameskipMode == MD_FRAMESKIP_ADAPTIVE) {
    if (s_mdAdaptiveSkipNextDraw) {
      s_mdAdaptiveSkipNextDraw = false;
      return false;
    }
    return true;
  }
  if (s_mdFrameskipMode == MD_FRAMESKIP_FIXED) {
    if (s_mdFixedSkipCreditQ8 >= 256) {
      s_mdFixedSkipCreditQ8 -= 256;
      return false;
    }
    s_mdFixedSkipCreditQ8 += kMdFixedFrameskipQ8[s_mdFixedFrameskipIndex];
  }
#if MD_GEOSKIP_FRAMESKIP
  if (s_mdFrameskipMode == MD_FRAMESKIP_GEOSKIP) {
    if (s_mdGeoSkipFramesToSkip > 0) {
      --s_mdGeoSkipFramesToSkip;
      return false;
    }
    s_mdGeoSkipFramesToSkip = 1;
  }
#endif
  return true;
}

static void md_frameskip_after_frame(bool lateSkip)
{
  if (s_mdFrameskipMode == MD_FRAMESKIP_ADAPTIVE) {
    s_mdAdaptiveSkipNextDraw = lateSkip;
  }
}

static inline bool md_frameskip_should_skip_z80(bool drawFrame)
{
#if MD_GEOSKIP_FRAMESKIP
  return s_mdFrameskipMode == MD_FRAMESKIP_GEOSKIP && !drawFrame;
#else
  (void)drawFrame;
  return false;
#endif
}

static bool md_key_down()
{
  return M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_1) ||
         M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_2) ||
         M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_3);
}

static bool md_key_up()
{
  return M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_1) ||
         M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_2);
}

static bool md_key_left()
{
  return M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1) ||
         M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_2);
}

static bool md_key_right()
{
  return M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1) ||
         M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_2);
}

static bool md_key_del()
{
  return M5Cardputer.Keyboard.isKeyPressed('\b');
}

enum MdG0Event : uint8_t {
  MD_G0_NONE = 0,
  MD_G0_SHORT,
  MD_G0_LONG,
};

static MdG0Event md_g0_poll_event()
{
  static bool wasDown = false;
  static bool longFired = false;
  static uint32_t downMs = 0;

  const bool down = M5Cardputer.BtnA.isPressed();
  const uint32_t now = millis();
  if (down && !wasDown) {
    wasDown = true;
    longFired = false;
    downMs = now;
    return MD_G0_NONE;
  }
  if (down && wasDown && !longFired && (now - downMs) >= 1000u) {
    longFired = true;
    return MD_G0_LONG;
  }
  if (!down && wasDown) {
    const uint32_t heldMs = now - downMs;
    wasDown = false;
    return (!longFired && heldMs >= 30u && heldMs < 700u) ? MD_G0_SHORT : MD_G0_NONE;
  }
  return MD_G0_NONE;
}

static int md_menu_row_count_for(MdMenuPage page)
{
  switch (page) {
    case MD_MENU_MAIN:
    case MD_MENU_FPS:
    case MD_MENU_AUDIO:
    case MD_MENU_VIDEO:
      return 2;
    case MD_MENU_HELP:
      return 1;
    default:
      return 2;
  }
}

static int md_menu_row_count()
{
  return md_menu_row_count_for(s_mdMenuPage);
}

static int md_menu_clamp_selection(MdMenuPage page, int selected)
{
  const int count = md_menu_row_count_for(page);
  if (count <= 0) return 0;
  if (selected < 0) return 0;
  if (selected >= count) return count - 1;
  return selected;
}

static void md_menu_stack_clear()
{
  s_mdMenuStackDepth = 0;
}

static void md_menu_enter_page(MdMenuPage page, int selected)
{
  if (s_mdMenuStackDepth < (int)(sizeof(s_mdMenuStack) / sizeof(s_mdMenuStack[0]))) {
    s_mdMenuStack[s_mdMenuStackDepth++] = {s_mdMenuPage, s_mdMenuSelected};
  }
  s_mdMenuPage = page;
  s_mdMenuSelected = md_menu_clamp_selection(page, selected);
}

static void md_menu_back()
{
  if (s_mdMenuStackDepth > 0) {
    const MdMenuStackEntry prev = s_mdMenuStack[--s_mdMenuStackDepth];
    s_mdMenuPage = prev.page;
    s_mdMenuSelected = md_menu_clamp_selection(prev.page, prev.selected);
    return;
  }
  s_mdMenuPage = MD_MENU_MAIN;
  s_mdMenuSelected = 0;
}

static void md_menu_handle_input(const Keyboard_Class::KeysState& ks)
{
  (void)ks;
  static bool prevUp = false;
  static bool prevDown = false;
  static bool prevLeft = false;
  static bool prevRight = false;
  static bool prevStart = false;
  static bool prevDel = false;
  static bool prevHelp = false;

  const bool up = md_key_up();
  const bool down = md_key_down();
  const bool left = md_key_left();
  const bool right = md_key_right();
  const bool start = M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_START) || ks.enter;
  const bool del = md_key_del() || ks.del;
  const bool help = M5Cardputer.Keyboard.isKeyPressed('h') || M5Cardputer.Keyboard.isKeyPressed('H');

  const bool upEdge = up && !prevUp;
  const bool downEdge = down && !prevDown;
  const bool leftEdge = left && !prevLeft;
  const bool rightEdge = right && !prevRight;
  const bool startEdge = start && !prevStart;
  const bool delEdge = del && !prevDel;
  const bool helpEdge = help && !prevHelp;

  prevUp = up;
  prevDown = down;
  prevLeft = left;
  prevRight = right;
  prevStart = start;
  prevDel = del;
  prevHelp = help;

  if (delEdge && s_mdMenuPage != MD_MENU_MAIN) {
    md_menu_back();
    md_update_menu_overlay();
    return;
  }

  if (s_mdMenuPage == MD_MENU_HELP) {
    if (leftEdge || upEdge) {
      md_cycle_help_page(-1);
      md_update_menu_overlay();
    } else if (rightEdge || downEdge || startEdge || helpEdge) {
      md_cycle_help_page(1);
      md_update_menu_overlay();
    }
    return;
  }

  if (helpEdge) {
    s_mdHelpSourcePage = s_mdMenuPage;
    s_mdHelpPageIndex = 0;
    md_menu_enter_page(MD_MENU_HELP, 0);
    md_update_menu_overlay();
    return;
  }

  if (upEdge || downEdge) {
    const int rowCount = md_menu_row_count();
    s_mdMenuSelected = (s_mdMenuSelected + (downEdge ? 1 : rowCount - 1)) % rowCount;
    md_update_menu_overlay();
  }

  if (s_mdMenuPage == MD_MENU_MAIN) {
    if (startEdge || leftEdge || rightEdge) {
      md_menu_enter_page((s_mdMenuSelected == 0) ? MD_MENU_AUDIO : MD_MENU_VIDEO, 0);
      md_update_menu_overlay();
    }
    return;
  }

  if (s_mdMenuPage == MD_MENU_AUDIO) {
    if (leftEdge || rightEdge || startEdge) {
      if (s_mdMenuSelected == 0) {
        md_toggle_pcm_sync();
      } else {
        md_toggle_wallclock_samples();
      }
      md_update_menu_overlay();
    }
    return;
  }

  if (s_mdMenuPage == MD_MENU_FPS) {
    if (leftEdge || rightEdge || startEdge) {
      md_set_fps_mode(s_mdMenuSelected == 0 ? MD_FPS_CORE : MD_FPS_VIDEO);
      md_update_menu_overlay();
    }
    return;
  }

  if (leftEdge || rightEdge) {
    if (s_mdMenuSelected == 0) {
      md_cycle_fps_mode(leftEdge ? -1 : 1);
    } else {
      md_cycle_frameskip(leftEdge ? -1 : 1);
    }
    md_update_menu_overlay();
  }

  if (startEdge) {
    if (s_mdMenuSelected == 0) {
      md_menu_enter_page(MD_MENU_FPS, (s_mdFpsOverlayMode == MD_FPS_VIDEO) ? 1 : 0);
    } else {
      md_cycle_frameskip(1);
    }
    md_update_menu_overlay();
  }
}

static void md_poll_ingame_menu()
{
  M5Cardputer.update();
  Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();
  share::checkCommonInput(ks, false);

  const MdG0Event g0 = md_g0_poll_event();
  if (g0 == MD_G0_LONG) {
    share::requestRestart();
    return;
  }
  if (g0 == MD_G0_SHORT) {
    s_mdMenuOpen = !s_mdMenuOpen;
    if (s_mdMenuOpen) {
      s_mdMenuPage = MD_MENU_MAIN;
      s_mdMenuSelected = 0;
      md_menu_stack_clear();
      md_release_gamepad_buttons();
    }
    md_update_menu_overlay();
  }

  if (s_mdMenuOpen) {
    md_menu_handle_input(ks);
  }
}

static void md_runtime_fps_record(bool drawFrame)
{
  const uint64_t now = (uint64_t)esp_timer_get_time();
  if (s_mdRuntimeFpsLastUs == 0) {
    s_mdRuntimeFpsLastUs = now;
  }
  ++s_mdRuntimeCoreFrames;
  if (drawFrame) {
    ++s_mdRuntimeVideoFrames;
  }
  const uint64_t elapsed = now - s_mdRuntimeFpsLastUs;
  if (elapsed >= 500000ULL) {
    s_mdCoreFps = (float)((double)s_mdRuntimeCoreFrames * 1000000.0 / (double)elapsed);
    s_mdVideoFps = (float)((double)s_mdRuntimeVideoFrames * 1000000.0 / (double)elapsed);
    s_mdRuntimeCoreFrames = 0;
    s_mdRuntimeVideoFrames = 0;
    s_mdRuntimeFpsLastUs = now;
    md_apply_fps_overlay();
  }
}

/* RUN ONE FRAME with VDP, M68K, Z80, Sound, etc. */
static bool run_one_frame() {
  const uint64_t t_start = micros();
  const bool drawFrame = md_frameskip_should_draw();
  const bool skipZ80Frame = md_frameskip_should_skip_z80(drawFrame);

  // Reset sound state
  #ifndef GENESIS_NO_SOUND
    ym2612_clock  = 0;
    ym2612_index  = 0;
    sn76489_clock = 0;
    sn76489_index = 0;
    zclk = 0;
  #endif

  // VDP and CPU setup for the frame
#if MD_BENCHMARK_LOGS_ENABLED
  uint64_t t_probe = md_bench_now_us();
#endif
  gwenesis_vdp_render_config();
#if MD_BENCHMARK_LOGS_ENABLED
  md_bench_add_u64(s_mdBench.vdpConfigUsTotal, t_probe);
#endif
  int cpu_deadline = 0;
  const unsigned h = screen_height ? screen_height : 224u;
  const int lines_per_frame = gwenesis_region_lines_per_frame();
  int hint_counter = gwenesis_vdp_regs[10];
#if MD_M68K_NO_HINT_BATCH_RUN
  bool m68kNoHintBatch = (REG0_LINE_INTERRUPT == 0);
  const int m68kBatchLines = (MD_M68K_NO_HINT_BATCH_LINES > 0) ? MD_M68K_NO_HINT_BATCH_LINES : 1;
#endif
  scan_line = 0;

  // Notify start of frame to display task
  if (g_scanQ) {
    ScanMsg b = {};
    b.type = MSG_BEGIN_FRAME;
    b.w = (uint16_t)FB_W;
    b.srcH = (uint16_t)h;
    b.format = SCANMSG_FORMAT_RGB565;
#if MD_RENDER_LOGS_ENABLED
    const BaseType_t ok = xQueueSend(g_scanQ, &b, 0);
    if (ok != pdTRUE) ++s_mdFrameDiag.beginSendFail;
#if MD_BENCHMARK_LOGS_ENABLED
    if (ok != pdTRUE) ++s_mdBench.beginSendFail;
#endif
#elif MD_BENCHMARK_LOGS_ENABLED
    const BaseType_t ok = xQueueSend(g_scanQ, &b, 0);
    if (ok != pdTRUE) ++s_mdBench.beginSendFail;
#else
    xQueueSend(g_scanQ, &b, 0);
#endif
  }

  // Per line emulation loop
#if MD_RENDER_LOGS_ENABLED || MD_BENCHMARK_LOGS_ENABLED
  uint32_t renderedLines = 0;
#endif
  while (scan_line < lines_per_frame) {
    cpu_deadline += VDP_CYCLES_PER_LINE;

    // Run M68K CPU. When HINT is disabled, batch a few lines to reduce fixed
    // per-call overhead while still flushing around VBlank and frame end.
#if MD_M68K_NO_HINT_BATCH_RUN
    const int next_scan_line = scan_line + 1;
    bool runM68kNow = true;
    if (m68kNoHintBatch) {
      const bool vblankBoundary = (scan_line == (int)h) || (scan_line == (int)h + 1);
      const bool frameBoundary = (next_scan_line >= lines_per_frame);
      const bool batchBoundary = ((next_scan_line % m68kBatchLines) == 0);
      runM68kNow = vblankBoundary || frameBoundary || batchBoundary;
    }
    if (runM68kNow) {
#if MD_BENCHMARK_LOGS_ENABLED
      t_probe = md_bench_now_us();
#endif
      md_xtensa_perf_m68k_run(cpu_deadline);
#if MD_BENCHMARK_LOGS_ENABLED
      md_bench_add_u64(s_mdBench.m68kUsTotal, t_probe);
#endif
      if (m68kNoHintBatch && REG0_LINE_INTERRUPT != 0) {
        m68kNoHintBatch = false;
      }
    }
#else
#if MD_BENCHMARK_LOGS_ENABLED
    t_probe = md_bench_now_us();
#endif
    md_xtensa_perf_m68k_run(cpu_deadline);
#if MD_BENCHMARK_LOGS_ENABLED
    md_bench_add_u64(s_mdBench.m68kUsTotal, t_probe);
#endif
#endif
    
    // Run Z80 and update YM2612 clock for sound
    #ifndef GENESIS_NO_SOUND
      if (genesis_audio_volume > 0) {
        if (!skipZ80Frame) {
#if MD_BENCHMARK_LOGS_ENABLED
          t_probe = md_bench_now_us();
#endif
          z80_run(cpu_deadline);
#if MD_BENCHMARK_LOGS_ENABLED
          md_bench_add_u64(s_mdBench.z80UsTotal, t_probe);
#endif
        }
#if MD_BENCHMARK_LOGS_ENABLED
        t_probe = md_bench_now_us();
#endif
        gwenesis_SN76489_run(cpu_deadline);
#if MD_BENCHMARK_LOGS_ENABLED
        md_bench_add_u64(s_mdBench.psgUsTotal, t_probe);
#endif
        genesis_sound_ym_set_target_clock(cpu_deadline);
      }
    #endif
    
    // VDP line rendering, if no frame skip and not the lines to skip
    if (drawFrame && (unsigned)scan_line < h && ((scan_line & 1) == g_field_ofs)) {
#if MD_BENCHMARK_LOGS_ENABLED
      t_probe = md_bench_now_us();
#endif
      gwenesis_vdp_render_line(scan_line);
#if MD_BENCHMARK_LOGS_ENABLED
      md_bench_add_u64(s_mdBench.renderUsTotal, t_probe);
#endif
#if MD_RENDER_LOGS_ENABLED || MD_BENCHMARK_LOGS_ENABLED
      ++renderedLines;
#endif
    }

    // On these lines, the line counter interrupt is reloaded
    if (scan_line == 0 || (unsigned)scan_line > h) {
      hint_counter = REG10_LINE_COUNTER;
    }

    // interrupt line counter
    if (--hint_counter < 0) {
      if ((REG0_LINE_INTERRUPT != 0) && (unsigned)scan_line <= h) {
        hint_pending = 1;
        if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
          m68k_update_irq(4);
      }
      hint_counter = REG10_LINE_COUNTER;
    }

    // vblank begin at the end of last rendered line
    if (scan_line == (int)h) {
      if (REG1_VBLANK_INTERRUPT != 0) {
        gwenesis_vdp_status |= STATUS_VIRQPENDING;
        m68k_set_irq(6);
      }
      #ifndef GENESIS_NO_SOUND
        if (genesis_audio_volume > 0 && !skipZ80Frame) z80_irq_line(1);
      #endif
    }

    #ifndef GENESIS_NO_SOUND
    if (scan_line == (int)h + 1) {
      if (genesis_audio_volume > 0 && !skipZ80Frame) z80_irq_line(0);
    }
    #endif

    ++scan_line;
  }

  m68k.cycles -= cpu_deadline; // reset cycle

  // Notify end of frame to display task
  if (g_scanQ) {
    ScanMsg e = {};
    e.type = MSG_END_FRAME;
    e.w = (uint16_t)FB_W;
    e.srcH = (uint16_t)h;
    e.format = SCANMSG_FORMAT_RGB565;
#if MD_RENDER_LOGS_ENABLED
    const BaseType_t ok = xQueueSend(g_scanQ, &e, 0);
    if (ok != pdTRUE) ++s_mdFrameDiag.endSendFail;
#if MD_BENCHMARK_LOGS_ENABLED
    if (ok != pdTRUE) ++s_mdBench.endSendFail;
#endif
#elif MD_BENCHMARK_LOGS_ENABLED
    const BaseType_t ok = xQueueSend(g_scanQ, &e, 0);
    if (ok != pdTRUE) ++s_mdBench.endSendFail;
#else
    xQueueSend(g_scanQ, &e, 0);
#endif
  }

  // Run SN76489 and push sound samples for this frame
#ifndef GENESIS_NO_SOUND
    if (genesis_audio_volume > 0) {
      const uint32_t audioFrameElapsedUs = (uint32_t)(micros() - t_start);
#if MD_BENCHMARK_LOGS_ENABLED
      t_probe = md_bench_now_us();
#endif
      gwenesis_SN76489_run(cpu_deadline);
      genesis_sound_submit_frame(audioFrameElapsedUs);
      genesis_sound_ym_set_target_clock(cpu_deadline);
#if MD_BENCHMARK_LOGS_ENABLED
      md_bench_add_u64(s_mdBench.audioSubmitUsTotal, t_probe);
#endif
    }
  #endif
  
  //  Frame time budget check for the next frame
  const uint32_t kFrameBudgetUs = 1000000u / (g_target_fps - 8); // 52FPS target
  const uint32_t elapsedUs = (uint32_t)(micros() - t_start);
  const bool lateSkip = elapsedUs > kFrameBudgetUs;
  md_frameskip_after_frame(lateSkip);
#if MD_RENDER_LOGS_ENABLED
  md_render_diag_record(drawFrame, skipZ80Frame, lateSkip, renderedLines, elapsedUs, kFrameBudgetUs, h, (uint32_t)lines_per_frame);
  md_render_diag_log_if_due();
#endif
#if MD_BENCHMARK_LOGS_ENABLED
  md_bench_record_frame(drawFrame, skipZ80Frame, lateSkip, renderedLines, elapsedUs, kFrameBudgetUs);
  md_bench_log_if_due();
#endif
  md_opcode_profile_log_if_due();
  md_m68k_category_profile_log_if_due();
  md_z80_prefix_profile_log_if_due();

#if MD_DETERMINISTIC_BENCH
  ++s_mdDeterministicBenchFrame;
#endif

#if EMU_LOG_MASTER_ENABLED && !MD_BENCHMARK_LOGS_ENABLED
  // FPS logging every second when the MD benchmark probe is disabled.
  frame_count++;
  uint64_t now = millis();
  if (now - last_fps_log_time >= 1000) {
    float fps = (frame_count * 1000.0f) / (now - last_fps_log_time);
    last_fps_log_time = now;
    frame_count = 0;
    // Log FPS + heap RAM 
    EMU_LOG("[FPS] ~%.1f fps | heap: %lu\n", fps, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
#endif
  return drawFrame;
}

/* Run genesis emulation with XIP mapped rom */
extern "C" void run_genesis(const uint8_t* rom, size_t len, const char* rom_name) {
  M5Cardputer.Display.setSwapBytes(true);
  md_render_diag_reset();
  md_bench_reset();
  md_xtensa_perf_reset();
  md_opcode_profile_reset();
  md_m68k_category_profile_reset();
  md_z80_prefix_profile_reset();
#if MD_DETERMINISTIC_BENCH
  s_mdDeterministicBenchFrame = 0;
  s_mdDeterministicBenchLimitLogged = 0;
  EMU_LOG("[MD][DETBENCH] enabled frames=%lu startAt=%lu startHold=%lu\n",
          (unsigned long)MD_DETERMINISTIC_BENCH_FRAMES,
          (unsigned long)MD_DETERMINISTIC_BENCH_START_AT_FRAME,
          (unsigned long)MD_DETERMINISTIC_BENCH_START_HOLD_FRAMES);
#endif
#if MD_BUS_PROBE_LOGS_ENABLED
  gwenesis_bus_probe_reset();
#endif

  // Load the XIP ROM into Gwenesis after the CPU RAMs exist.
  genesis_alloc_cpu_buffers();
  load_cartridge((unsigned char*)rom, len);
  g_target_fps = gwenesis_region_refresh_rate();

  // Save
  gwenesis_init_sram((uint8_t*)rom, (uint32_t)len);
  EMU_LOG("[SRAM] enabled=%d start=%08lX end=%08lX\n",
          SRAM_ENABLED,
          (unsigned long)SRAM_START,
          (unsigned long)SRAM_END);
  #ifndef GENESIS_NO_SOUND
    genesis_sound_set_sram_profile(md_has_sram());
    genesis_sound_configure_timing(g_target_fps,
                                   gwenesis_region_audio_rate(),
                                   gwenesis_region_audio_divisor(),
                                   gwenesis_region_lines_per_frame());
    const int mdCoreSamples = genesis_sound_get_core_samples_per_frame();
    const int mdOutSamples = genesis_sound_get_output_samples_per_frame();
    const int mdOutRate = genesis_sound_get_output_rate();
    const int mdChunkCap = genesis_sound_get_chunk_cap();
    const int mdPoolSlots = genesis_sound_get_pool_slots();
  #else
    const int mdCoreSamples = 0;
    const int mdOutSamples = 0;
    const int mdOutRate = 0;
    const int mdChunkCap = 0;
    const int mdPoolSlots = 0;
  #endif
  EMU_LOG("[MD][REGION] %s fps=%d lines=%d audio coreRate=%d divisor=%d coreSamples=%d outRate=%d outSamples=%d\n",
          gwenesis_region_name(),
          g_target_fps,
          gwenesis_region_lines_per_frame(),
          gwenesis_region_audio_rate(),
          gwenesis_region_audio_divisor(),
          mdCoreSamples,
          mdOutRate,
          mdOutSamples);
  EMU_LOG("[MD][AUDIOPROFILE] sram=%d outRate=%d chunkCap=%d pool=%d\n",
          md_has_sram() ? 1 : 0,
          mdOutRate,
          mdChunkCap,
          mdPoolSlots);
  md_runtime_options_reset();
  md_options_make_path(rom_name);
  genesis_save_init(rom_name);
  md_load_sram_with_sd();
  md_options_load_with_sd();
  md_enter_sd_off_gameplay();

  // SRAM ROMs use a smaller audio profile after VRAM keeps its 64 KiB block.
  genesis_alloc_vram_buffer();
  #ifndef GENESIS_NO_SOUND
    genesis_alloc_audio_buffers();
  #endif
  genesis_alloc_vdp_buffers();

  // header ASCII "SEGA"0x100
  EMU_LOG("[ROM] ptr=%p size=%u\n", rom, (unsigned)len);
  if (len >= 0x120) {
    EMU_LOG("[ROM] hdr[0x100..0x10F]= ");
    for (int i=0; i<16; ++i) EMU_LOG("%02X ", FETCH8ROM(0x100+i));
    EMU_LOG("\n");
  }

  // Init Gwenesis
  power_on();
  reset_emulation();

  // Init display and sound
  genesis_display_init();
  genesis_display_start();
  #ifndef GENESIS_NO_SOUND
    genesis_sound_init();
    genesis_sound_ym_init();
    genesis_sound_ym_start();
  #endif

  screen_width = REG12_MODE_H40 ? 320 : 256;
  screen_height = REG1_PAL ? 240 : 224;
  s_mdMenuOpen = false;
  s_mdMenuSelected = 0;
  s_mdMenuPage = MD_MENU_MAIN;
  md_menu_stack_clear();
  md_apply_fps_overlay();
  genesis_display_set_menu_overlay(false,
                                   s_mdMenuSelected,
                                   "CONFIG MENU",
                                   "AUDIO",
                                   "",
                                   "VIDEO",
                                   "",
                                   "",
                                   "",
                                   "START enter",
                                   "GO close menu");
  
  // Main emulation loop with frame pacing
  uint64_t next_frame_us = esp_timer_get_time();
  for (;;) {
    const int fps = g_target_fps; // snapshot
    const uint32_t frame_us = (fps > 0) ? (1000000u / (uint32_t)fps) : 0u;

    md_poll_ingame_menu();
    if (share::restartRequested()) {
      break;
    }
    if (s_mdMenuOpen) {
      md_release_gamepad_buttons();
      genesis_save_tick();
      share::sleep_until_us((uint64_t)esp_timer_get_time() + 16000u);
      next_frame_us = esp_timer_get_time();
      continue;
    }

    // Emulate one frame
    const bool drewFrame = run_one_frame();
    md_runtime_fps_record(drewFrame);
#if MD_DETERMINISTIC_BENCH && (MD_DETERMINISTIC_BENCH_FRAMES > 0)
    if (s_mdDeterministicBenchFrame >= (uint32_t)MD_DETERMINISTIC_BENCH_FRAMES) {
      if (!s_mdDeterministicBenchLimitLogged) {
        EMU_LOG("[MD][DETBENCH] frame marker reached frame=%lu exit=%u\n",
                (unsigned long)s_mdDeterministicBenchFrame,
                (unsigned)MD_DETERMINISTIC_BENCH_EXIT_ON_LIMIT);
        s_mdDeterministicBenchLimitLogged = 1;
      }
#if MD_DETERMINISTIC_BENCH_EXIT_ON_LIMIT
      break;
#endif
    }
#endif
    if (share::restartRequested()) {
      break;
    }
    genesis_save_tick();

    // Pacing to maintain target FPS
    if (frame_us > 0) {
      next_frame_us += frame_us;

      // If we are late, adjust next_frame_us to now + frame_us
      int64_t now = (int64_t)esp_timer_get_time();
      if (now - (int64_t)next_frame_us > (int64_t)frame_us) {
        next_frame_us = (uint64_t)now + frame_us;
      }

      share::sleep_until_us(next_frame_us);
    } else {
      taskYIELD();
    }
  }

#if MD_RENDER_LOGS_ENABLED
  md_render_diag_log_if_due(true);
#endif
#if MD_BENCHMARK_LOGS_ENABLED
  md_bench_log_if_due(true);
#endif
  md_opcode_profile_log_if_due(true);
  md_m68k_category_profile_log_if_due(true);
  md_z80_prefix_profile_log_if_due(true);
  md_clean_teardown_and_save();
}
