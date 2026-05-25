#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "genesis/run_genesis.h"
#include "genesis_sound.h"
#include "genesis_display.h"
#include "genesis_save.h"
#include "genesis/gwenesis/bus/gwenesis_bus.h"
#include "share/input.h"
#include "share/sd_control.h"
#include "share/sd_gameplay_guard.h"
#include "share/utils.h"
#include "share/emu_log_cpp.h"
#include "compat/arduino_compat.h"
#include <M5Cardputer.h>

extern "C" {
  #include "genesis/gwenesis/cpus/M68K/m68k.h"
}

#if EMU_LOG_MASTER_ENABLED
static uint32_t frame_count = 0;
static uint64_t last_fps_log_time = 0;
#endif
static volatile int g_target_fps = 60;
static bool s_draw_toggle = false;  // for skipping frames
static volatile bool s_skipZ80Next = false; // skip Z80 on next frame if true
extern int genesisZoomPercent;

#ifndef MD_BENCH_NO_FRAME_SKIP
#define MD_BENCH_NO_FRAME_SKIP 0
#endif

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

/* Allocate core buffers: VRAM, M68K RAM, Z80 RAM, VDP buffers */
static void genesis_alloc_core_buffers(void) {
  ensure_alloc((void**)&VRAM, VRAM_MAX_SIZE, "VRAM");          // 64 KiB
  ensure_alloc((void**)&M68K_RAM, MAX_RAM_SIZE, "M68K RAM");   // 64K main RAM
  ensure_alloc((void**)&ZRAM, MAX_Z80_RAM_SIZE, "Z80 RAM");    // 8K Z80 RAM
  gwenesis_vdp_allocate_buffers();
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

  genesis_display_stop();
  genesis_sound_ym_stop();
  genesis_sound_shutdown();

  if (md_has_sram()) {
    if (share_sd_gameplay_mount("MD", "save")) {
      genesis_save_force_flush();
    } else {
      EMU_LOG("[GEN][SAVE] SD remount failed, SRAM not saved\n");
    }
  } else {
    EMU_LOG("[MD][SD] no SRAM, skip SD remount/save\n");
  }

#ifdef MD_SD_OFF_DURING_GAMEPLAY
  share_sd_gameplay_close_if_mounted("MD", "save");
#endif

  genesis_save_shutdown();
  md_reset_quit_controls();
  EMU_LOG("[MD][QUIT] clean teardown done\n");
}

/* RUN ONE FRAME with VDP, M68K, Z80, Sound, etc. */
static void run_one_frame() {
  const uint64_t t_start = micros();
#if MD_BENCH_NO_FRAME_SKIP
  const bool drawFrame = true;
  const bool skipZ80 = false;
  s_skipZ80Next = false;
#else
  const bool drawFrame = (!s_skipZ80Next) && (s_draw_toggle = !s_draw_toggle); // frame skip logic
  const bool skipZ80 = s_skipZ80Next;   // snapshot
  s_skipZ80Next = false;
#endif

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
  scan_line = 0;

  // Notify start of frame to display task
  if (g_scanQ) {
    ScanMsg b = { MSG_BEGIN_FRAME, 0, (uint16_t)FB_W, (uint16_t)h, {0} };
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

    // Run M68K CPU
#if MD_BENCHMARK_LOGS_ENABLED
    t_probe = md_bench_now_us();
#endif
    m68k_run(cpu_deadline);
#if MD_BENCHMARK_LOGS_ENABLED
    md_bench_add_u64(s_mdBench.m68kUsTotal, t_probe);
#endif
    
    // Run Z80 and update YM2612 clock for sound
    #ifndef GENESIS_NO_SOUND
      if (genesis_audio_volume > 0) {
        if (!skipZ80) {
#if MD_BENCHMARK_LOGS_ENABLED
          t_probe = md_bench_now_us();
#endif
          z80_run(cpu_deadline);
#if MD_BENCHMARK_LOGS_ENABLED
          md_bench_add_u64(s_mdBench.z80UsTotal, t_probe);
          t_probe = md_bench_now_us();
#endif
          gwenesis_SN76489_run(cpu_deadline);
#if MD_BENCHMARK_LOGS_ENABLED
          md_bench_add_u64(s_mdBench.psgUsTotal, t_probe);
#endif
          genesis_sound_ym_set_target_clock(cpu_deadline);
        }
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
        if (genesis_audio_volume > 0) z80_irq_line(1);
      #endif
    }

    #ifndef GENESIS_NO_SOUND
    if (scan_line == (int)h + 1) {
      if (genesis_audio_volume > 0) z80_irq_line(0);
    }
    #endif

    ++scan_line;
  }

  m68k.cycles -= cpu_deadline; // reset cycle

  // Notify end of frame to display task
  if (g_scanQ) {
    ScanMsg e = { MSG_END_FRAME, 0, (uint16_t)FB_W, (uint16_t)h, {0} };
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
#if MD_BENCHMARK_LOGS_ENABLED
      t_probe = md_bench_now_us();
#endif
      gwenesis_SN76489_run(cpu_deadline);
      genesis_sound_submit_frame();
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
#if !MD_BENCH_NO_FRAME_SKIP
  if (lateSkip) {
    s_skipZ80Next = true;  // we are late, skip Z80 next frame
  }
#endif
#if MD_RENDER_LOGS_ENABLED
  md_render_diag_record(drawFrame, skipZ80, lateSkip, renderedLines, elapsedUs, kFrameBudgetUs, h, (uint32_t)lines_per_frame);
  md_render_diag_log_if_due();
#endif
#if MD_BENCHMARK_LOGS_ENABLED
  md_bench_record_frame(drawFrame, skipZ80, lateSkip, renderedLines, elapsedUs, kFrameBudgetUs);
  md_bench_log_if_due();
#endif
  md_opcode_profile_log_if_due();

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
}

/* Run genesis emulation with XIP mapped rom */
extern "C" void run_genesis(const uint8_t* rom, size_t len, const char* rom_name) {
  M5Cardputer.Display.setSwapBytes(true);
  md_render_diag_reset();
  md_bench_reset();
  md_opcode_profile_reset();
#if MD_BUS_PROBE_LOGS_ENABLED
  gwenesis_bus_probe_reset();
#endif

  // Allocate buffers
  genesis_alloc_core_buffers();
  #ifndef GENESIS_NO_SOUND
    genesis_alloc_audio_buffers();
  #endif

  
  // Load the xip ROM into Gwenesis
  load_cartridge((unsigned char*)rom, len);
  g_target_fps = gwenesis_region_refresh_rate();
  #ifndef GENESIS_NO_SOUND
    genesis_sound_configure_timing(g_target_fps,
                                   gwenesis_region_audio_rate(),
                                   gwenesis_region_audio_divisor(),
                                   gwenesis_region_lines_per_frame());
    const int mdCoreSamples = genesis_sound_get_core_samples_per_frame();
    const int mdOutSamples = genesis_sound_get_output_samples_per_frame();
  #else
    const int mdCoreSamples = 0;
    const int mdOutSamples = 0;
  #endif
  EMU_LOG("[MD][REGION] %s fps=%d lines=%d audio coreRate=%d divisor=%d coreSamples=%d outRate=%d outSamples=%d\n",
          gwenesis_region_name(),
          g_target_fps,
          gwenesis_region_lines_per_frame(),
          gwenesis_region_audio_rate(),
          gwenesis_region_audio_divisor(),
          mdCoreSamples,
          AUDIO_SR,
          mdOutSamples);

  // Save
  gwenesis_init_sram((uint8_t*)rom, (uint32_t)len);
  EMU_LOG("[SRAM] enabled=%d start=%08lX end=%08lX\n",
          SRAM_ENABLED,
          (unsigned long)SRAM_START,
          (unsigned long)SRAM_END);
  genesis_save_init(rom_name);
  md_load_sram_with_sd();
  md_enter_sd_off_gameplay();

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
  
  // Main emulation loop with frame pacing
  uint64_t next_frame_us = esp_timer_get_time();
  for (;;) {
    const int fps = g_target_fps; // snapshot
    const uint32_t frame_us = (fps > 0) ? (1000000u / (uint32_t)fps) : 0u;

    // Emulate one frame
    run_one_frame();
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
  md_clean_teardown_and_save();
}
