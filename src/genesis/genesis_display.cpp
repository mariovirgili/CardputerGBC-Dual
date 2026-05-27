#include "genesis_display.h"
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "compat/arduino_compat.h"
#include <M5Cardputer.h>
#include "share/emu_log_cpp.h"

extern "C" {
  // Gwenesis VDP
  extern unsigned int screen_height;
  extern unsigned short *CRAM565;
}

// Globals
QueueHandle_t g_scanQ = nullptr;
TaskHandle_t  g_displayTaskHandle = nullptr;
#if MD_DISPLAY_SCANLINE_RING
static QueueHandle_t g_scanFreeQ = nullptr;
static ScanLineSlot *s_scanSlots = nullptr;
#endif
int g_dstW, g_dstH;
int g_viewX0, g_viewY0;
int g_viewW, g_viewH;
bool g_inFrame = false;
int g_prevDstY = 0;
int g_srcH_cached = -1;
int g_field_ofs = 0;  // 0 = odd, 1 = even
uint16_t *s_lineImg  = nullptr;
uint16_t *s_lineFull = nullptr;
int s_capImg  = 0;
int s_capFull = 0;
static uint16_t *s_xmap = nullptr;
static int s_xmap_srcW  = -1, s_xmap_dstW = -1;
static int s_xmap_roiX0 = -1, s_xmap_roiW = -1;
static int s_roiX0 = 0, s_roiY0 = 0, s_roiW = 0, s_roiH = 0;

int genesisZoomPercent = 110;
bool fullscreenMode __attribute__((weak)) = true;

#if MD_RENDER_LOGS_ENABLED
struct MdDisplayDiagStats {
  uint64_t lastLogMs = 0;
  uint32_t beginRecv = 0;
  uint32_t endRecv = 0;
  uint32_t scanSent = 0;
  uint32_t scanRecv = 0;
  uint32_t queueFullBefore = 0;
  uint32_t sendFail = 0;
  uint32_t sendWaitOver1ms = 0;
  uint32_t sendWaitMaxUs = 0;
  uint64_t sendWaitTotalUs = 0;
  uint32_t lcdRows = 0;
  uint32_t maxLinesToPush = 0;
  uint32_t roiChanges = 0;
};

static MdDisplayDiagStats s_mdDisplayDiag;

static inline uint64_t md_display_now_ms()
{
  return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static inline void md_display_diag_init_once()
{
  if (s_mdDisplayDiag.lastLogMs == 0) {
    s_mdDisplayDiag.lastLogMs = md_display_now_ms();
  }
}

static inline void md_display_diag_log_if_due(bool force = false)
{
  md_display_diag_init_once();
  MdDisplayDiagStats& d = s_mdDisplayDiag;
  const uint64_t now = md_display_now_ms();
  if (!force && (now - d.lastLogMs) < 1000ULL) return;
  if (d.beginRecv == 0 && d.endRecv == 0 && d.scanSent == 0 && d.scanRecv == 0) {
    d.lastLogMs = now;
    return;
  }
  const uint32_t sendAvgUs = d.scanSent ? (uint32_t)(d.sendWaitTotalUs / d.scanSent) : 0;
  MD_RENDER_LOG("display begin=%lu end=%lu scan sent/recv=%lu/%lu qFullBefore=%lu sendFail=%lu waitUs avg/max=%lu/%lu waitOver1ms=%lu lcdRows=%lu maxLinesToPush=%lu roiChanges=%lu dst=%dx%d view=%dx%d",
                (unsigned long)d.beginRecv,
                (unsigned long)d.endRecv,
                (unsigned long)d.scanSent,
                (unsigned long)d.scanRecv,
                (unsigned long)d.queueFullBefore,
                (unsigned long)d.sendFail,
                (unsigned long)sendAvgUs,
                (unsigned long)d.sendWaitMaxUs,
                (unsigned long)d.sendWaitOver1ms,
                (unsigned long)d.lcdRows,
                (unsigned long)d.maxLinesToPush,
                (unsigned long)d.roiChanges,
                g_dstW,
                g_dstH,
                g_viewW,
                g_viewH);
  d = MdDisplayDiagStats{};
  d.lastLogMs = now;
}

static inline BaseType_t md_display_send_scan_msg(QueueHandle_t q, const ScanMsg* msg, TickType_t ticks)
{
  md_display_diag_init_once();
  if (uxQueueSpacesAvailable(q) == 0) {
    ++s_mdDisplayDiag.queueFullBefore;
  }
  const uint64_t t0 = esp_timer_get_time();
  const BaseType_t ok = xQueueSend(q, msg, ticks);
  const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - t0);
  ++s_mdDisplayDiag.scanSent;
  s_mdDisplayDiag.sendWaitTotalUs += elapsed;
  if (elapsed > s_mdDisplayDiag.sendWaitMaxUs) s_mdDisplayDiag.sendWaitMaxUs = elapsed;
  if (elapsed > 1000) ++s_mdDisplayDiag.sendWaitOver1ms;
  if (ok != pdTRUE) ++s_mdDisplayDiag.sendFail;
  md_display_diag_log_if_due();
  return ok;
}
#endif

#if MD_DISPLAY_SCANLINE_RING
static inline bool md_display_take_scan_slot(uint8_t *slot)
{
  return g_scanFreeQ &&
         xQueueReceive(g_scanFreeQ, slot, portMAX_DELAY) == pdTRUE;
}

static inline void md_display_release_scan_slot(uint8_t slot)
{
  if (g_scanFreeQ && slot < SCANLINE_QUEUE_DEPTH) {
    xQueueSend(g_scanFreeQ, &slot, 0);
  }
}

static inline ScanLineSlot *md_display_msg_slot(const ScanMsg& m)
{
  if (!s_scanSlots || m.slot >= SCANLINE_QUEUE_DEPTH) return nullptr;
  return &s_scanSlots[m.slot];
}
#endif

/* Memory set and copy helpers */
static inline void memset16(uint16_t *dst, uint16_t v, int count) {
  for (int i = 0; i < count; ++i) dst[i] = v;
}
static inline void memcpy16(uint16_t *dst, const uint16_t *src, int count) {
  memcpy(dst, src, count * sizeof(uint16_t));
}

/* helpers ROI & xmap */
static inline int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/* Calculate centered Region of Interest */
static inline void compute_centered_roi(int srcW, int srcH) {
  int zp = (genesisZoomPercent <= 0) ? 100 : genesisZoomPercent;
  int roiW = (int)((int64_t)srcW * 100 / zp);
  int roiH = (int)((int64_t)srcH * 100 / zp);

  // Bornes
  roiW = clampi(roiW, 16, srcW);
  roiH = clampi(roiH, 16, srcH);

  int roiX0 = (srcW - roiW) / 2;
  int roiY0 = (srcH - roiH) / 2;

  s_roiX0 = roiX0;
  s_roiY0 = roiY0;
  s_roiW  = roiW;
  s_roiH  = roiH;
}

/* Ensure xmap ROI */
static inline bool ensure_xmap_roi(int srcW, int dstW, int roiX0, int roiW) {
  if (s_xmap &&
      s_xmap_srcW == srcW &&
      s_xmap_dstW == dstW &&
      s_xmap_roiX0 == roiX0 &&
      s_xmap_roiW  == roiW) {
    return true;
  }
  free(s_xmap);
  s_xmap = (uint16_t*)heap_caps_malloc(dstW * sizeof(uint16_t), MALLOC_CAP_8BIT);
  if (!s_xmap) {
    s_xmap_srcW = -1;
    s_xmap_dstW = -1;
    s_xmap_roiX0 = -1;
    s_xmap_roiW = -1;
    MD_RENDER_LOG("xmap alloc failed dstW=%d", dstW);
    return false;
  }

  s_xmap_srcW = srcW;
  s_xmap_dstW = dstW;
  s_xmap_roiX0 = roiX0;
  s_xmap_roiW  = roiW;

  // x -> roiX0 + x * roiW / dstW
  for (int x = 0; x < dstW; ++x) {
    s_xmap[x] = (uint16_t)(roiX0 + (int)((int64_t)x * roiW / dstW));
  }
  return true;
}

/* Allocate buffers for line rendering */
static inline void allocate_line_buffers() {
  if (g_viewW > s_capImg) {
    free(s_lineImg);
    s_lineImg  = (uint16_t*)heap_caps_malloc(g_viewW * sizeof(uint16_t),
                                             MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_capImg   = s_lineImg ? g_viewW : 0;
    if (!s_lineImg) {
      MD_RENDER_LOG("lineImg alloc failed width=%d", g_viewW);
    }
  }
  if (g_dstW > s_capFull) {
    free(s_lineFull);
    s_lineFull = (uint16_t*)heap_caps_malloc(g_dstW * sizeof(uint16_t),
                                             MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_capFull  = s_lineFull ? g_dstW : 0;
    if (!s_lineFull) {
      MD_RENDER_LOG("lineFull alloc failed width=%d", g_dstW);
    }
  }
}

/* Initialize display subsystem */
extern "C" void genesis_display_init(void) {
  g_dstW = M5.Lcd.width();   // 240
  g_dstH = M5.Lcd.height();  // 135

  // Screen viewport defaults cardputer
  g_viewW = g_dstW;
  g_viewH = g_dstH;
  g_viewX0 = 0;
  g_viewY0 = 0;

  // Buffers
  if (!s_lineFull) s_lineFull = (uint16_t*)heap_caps_malloc(g_dstW * sizeof(uint16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!s_lineImg)  s_lineImg  = (uint16_t*)heap_caps_malloc(g_viewW * sizeof(uint16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

/* Display task */
void display_task(void* arg) {
  allocate_line_buffers();
  int prevDstY = 0;
  bool inFrame = false;
  int cachedSrcH = -1;
  int roiInitForSrcW = -1;

  for (;;) {
    ScanMsg m;
    if (xQueueReceive(g_scanQ, &m, portMAX_DELAY) != pdTRUE) continue;

    if (m.type == MSG_BEGIN_FRAME) {
#if MD_RENDER_LOGS_ENABLED
      ++s_mdDisplayDiag.beginRecv;
#endif
      cachedSrcH = m.srcH;
      compute_centered_roi(/*srcW=*/FB_W, /*srcH=*/cachedSrcH); 
      roiInitForSrcW = -1;

      M5.Lcd.startWrite();
      inFrame = true;
      prevDstY = g_viewY0;
      continue;
    }

    if (m.type == MSG_END_FRAME) {
#if MD_RENDER_LOGS_ENABLED
      ++s_mdDisplayDiag.endRecv;
      md_display_diag_log_if_due();
#endif
      if (inFrame) {
        M5.Lcd.endWrite();
        inFrame = false;
      }
      continue;
    }

    if (roiInitForSrcW != (int)m.w) {
      compute_centered_roi((int)m.w, (int)m.srcH);
      ensure_xmap_roi(/*srcW*/ m.w, /*dstW*/ g_viewW, /*roiX0*/ s_roiX0, /*roiW*/ s_roiW);
      roiInitForSrcW = (int)m.w;
#if MD_RENDER_LOGS_ENABLED
      ++s_mdDisplayDiag.roiChanges;
#endif
    } else {
      ensure_xmap_roi(/*srcW*/ m.w, /*dstW*/ g_viewW, /*roiX0*/ s_roiX0, /*roiW*/ s_roiW);
    }

    if (!s_lineImg || !s_lineFull) {
      allocate_line_buffers();
      if (!s_lineImg || !s_lineFull) {
#if MD_DISPLAY_SCANLINE_RING
        md_display_release_scan_slot(m.slot);
#endif
        continue;
      }
    }

#if MD_RENDER_LOGS_ENABLED
    ++s_mdDisplayDiag.scanRecv;
#endif

    if (m.format == SCANMSG_FORMAT_INDEX8) {
#if MD_DISPLAY_SCANLINE_RING
      ScanLineSlot *slot = md_display_msg_slot(m);
      if (!slot) {
        md_display_release_scan_slot(m.slot);
        continue;
      }
      const uint8_t *idx = slot->indexed.idx;
      const uint16_t *palette = slot->indexed.palette;
#else
      const uint8_t *idx = m.indexed.idx;
      const uint16_t *palette = m.indexed.palette;
#endif
      if (m.w == g_viewW && s_roiX0 == 0 && s_roiW == m.w) {
#if MD_RENDER_INDEX_UNROLL
        int x = 0;
        for (; x + 3 < g_viewW; x += 4) {
          s_lineImg[x + 0] = palette[idx[x + 0] & 0x3F];
          s_lineImg[x + 1] = palette[idx[x + 1] & 0x3F];
          s_lineImg[x + 2] = palette[idx[x + 2] & 0x3F];
          s_lineImg[x + 3] = palette[idx[x + 3] & 0x3F];
        }
        for (; x < g_viewW; ++x) {
          s_lineImg[x] = palette[idx[x] & 0x3F];
        }
#else
        for (int x = 0; x < g_viewW; ++x) {
          s_lineImg[x] = palette[idx[x] & 0x3F];
        }
#endif
      } else {
        const uint16_t *xmap = s_xmap;
#if MD_RENDER_INDEX_UNROLL
        int x = 0;
        for (; x + 3 < g_viewW; x += 4) {
          const int srcX0 = xmap ? xmap[x + 0] : (s_roiX0 + (int)((int64_t)(x + 0) * s_roiW / g_viewW));
          const int srcX1 = xmap ? xmap[x + 1] : (s_roiX0 + (int)((int64_t)(x + 1) * s_roiW / g_viewW));
          const int srcX2 = xmap ? xmap[x + 2] : (s_roiX0 + (int)((int64_t)(x + 2) * s_roiW / g_viewW));
          const int srcX3 = xmap ? xmap[x + 3] : (s_roiX0 + (int)((int64_t)(x + 3) * s_roiW / g_viewW));
          s_lineImg[x + 0] = palette[idx[srcX0] & 0x3F];
          s_lineImg[x + 1] = palette[idx[srcX1] & 0x3F];
          s_lineImg[x + 2] = palette[idx[srcX2] & 0x3F];
          s_lineImg[x + 3] = palette[idx[srcX3] & 0x3F];
        }
        for (; x < g_viewW; ++x) {
          const int srcX = xmap ? xmap[x] : (s_roiX0 + (int)((int64_t)x * s_roiW / g_viewW));
          s_lineImg[x] = palette[idx[srcX] & 0x3F];
        }
#else
        for (int x = 0; x < g_viewW; ++x) {
          const int srcX = xmap ? xmap[x] : (s_roiX0 + (int)((int64_t)x * s_roiW / g_viewW));
          s_lineImg[x] = palette[idx[srcX] & 0x3F];
        }
#endif
      }
    } else if (m.w == g_viewW && s_roiX0 == 0 && s_roiW == m.w) {
#if MD_DISPLAY_SCANLINE_RING
      ScanLineSlot *slot = md_display_msg_slot(m);
      if (!slot) {
        md_display_release_scan_slot(m.slot);
        continue;
      }
      memcpy16(s_lineImg, slot->data, g_viewW);
#else
      memcpy16(s_lineImg, m.data, g_viewW);
#endif
    } else {
      const uint16_t *xmap = s_xmap;
#if MD_DISPLAY_SCANLINE_RING
      ScanLineSlot *slot = md_display_msg_slot(m);
      if (!slot) {
        md_display_release_scan_slot(m.slot);
        continue;
      }
      const uint16_t *src = slot->data;
#else
      const uint16_t *src = m.data;
#endif
      for (int x = 0; x < g_viewW; ++x) {
        const int srcX = xmap ? xmap[x] : (s_roiX0 + (int)((int64_t)x * s_roiW / g_viewW));
        s_lineImg[x] = src[srcX];
      }
    }

    const int leftBlack  = g_viewX0;
    const int rightBlack = g_dstW - (g_viewX0 + g_viewW);

    if (leftBlack > 0)  memset16(s_lineFull, 0, leftBlack);
    memcpy16(s_lineFull + leftBlack, s_lineImg, g_viewW);
    if (rightBlack > 0) memset16(s_lineFull + leftBlack + g_viewW, 0, rightBlack);

    int srcLineP1 = m.line + 1;
    int relP1     = srcLineP1 - s_roiY0;
    relP1 = clampi(relP1, 0, s_roiH);
    int dstY_end  = g_viewY0 + (int)((int64_t)relP1 * g_viewH / s_roiH);

    int linesToPush = dstY_end - prevDstY;
#if MD_RENDER_LOGS_ENABLED
    if (linesToPush > s_mdDisplayDiag.maxLinesToPush) {
      s_mdDisplayDiag.maxLinesToPush = (uint32_t)linesToPush;
    }
#endif

    while (linesToPush > 0) {
      int chunk = (linesToPush > 16) ? 16 : linesToPush;
      M5.Lcd.setAddrWindow(0, prevDstY, g_dstW, chunk);
      for (int i = 0; i < chunk; ++i) {
        M5.Lcd.writePixels(s_lineFull, g_dstW);
      }
      prevDstY    += chunk;
      linesToPush -= chunk;
#if MD_RENDER_LOGS_ENABLED
      s_mdDisplayDiag.lcdRows += (uint32_t)chunk;
#endif
      if ((prevDstY & 31) == 0) vTaskDelay(0);
    }

    // Yield
    if ((m.line & 31) == 31) vTaskDelay(0);

#if MD_DISPLAY_SCANLINE_RING
    md_display_release_scan_slot(m.slot);
#endif
  }
}

/* Start the display task */
extern "C" void genesis_display_start(void) {
#if MD_DISPLAY_SCANLINE_RING
  if (!s_scanSlots) {
    s_scanSlots = (ScanLineSlot*)heap_caps_calloc(SCANLINE_QUEUE_DEPTH,
                                                  sizeof(ScanLineSlot),
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_scanSlots) {
      EMU_LOG("[GENESIS][VIDEO] scanline ring alloc failed\n");
      return;
    }
  }
  if (!g_scanFreeQ) {
    g_scanFreeQ = xQueueCreate(SCANLINE_QUEUE_DEPTH, sizeof(uint8_t));
    if (!g_scanFreeQ) {
      EMU_LOG("[GENESIS][VIDEO] free queue create failed\n");
      free(s_scanSlots);
      s_scanSlots = nullptr;
      return;
    }
    for (uint8_t i = 0; i < SCANLINE_QUEUE_DEPTH; ++i) {
      xQueueSend(g_scanFreeQ, &i, 0);
    }
  }
#endif
  if (!g_scanQ) {
    g_scanQ = xQueueCreate(SCANLINE_QUEUE_DEPTH, sizeof(ScanMsg));
    if (!g_scanQ) {
      EMU_LOG("[GENESIS][VIDEO] queue create failed\n");
#if MD_DISPLAY_SCANLINE_RING
      vQueueDelete(g_scanFreeQ);
      g_scanFreeQ = nullptr;
      free(s_scanSlots);
      s_scanSlots = nullptr;
#endif
      return;
    }
  }
  if (!g_displayTaskHandle) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      display_task, "DisplayTask",
      3192, nullptr, 6, &g_displayTaskHandle,
      0 /* core  */
    );
    if (ok != pdPASS) {
      EMU_LOG("[GENESIS][VIDEO] task create failed\n");
      vQueueDelete(g_scanQ); g_scanQ = nullptr;
#if MD_DISPLAY_SCANLINE_RING
      vQueueDelete(g_scanFreeQ); g_scanFreeQ = nullptr;
      free(s_scanSlots); s_scanSlots = nullptr;
#endif
    }
  }
}

/* Stop the display task */
extern "C" void genesis_display_stop(void) {
  if (g_displayTaskHandle) {
    vTaskDelete(g_displayTaskHandle);
    g_displayTaskHandle = nullptr;
  }
  if (g_scanQ) {
    vQueueDelete(g_scanQ);
    g_scanQ = nullptr;
  }
#if MD_DISPLAY_SCANLINE_RING
  if (g_scanFreeQ) {
    vQueueDelete(g_scanFreeQ);
    g_scanFreeQ = nullptr;
  }
  free(s_scanSlots);
  s_scanSlots = nullptr;
#endif
}

/* Begin a new frame */
extern "C" void genesis_display_begin_frame(uint16_t srcH) {
  if (!g_scanQ) return;
  // Reset cached ROI 
  s_roiX0 = 0; s_roiY0 = 0; s_roiW = FB_W; s_roiH = srcH;
  ScanMsg b = {};
  b.type = MSG_BEGIN_FRAME;
  b.w = (uint16_t)FB_W;
  b.srcH = srcH;
  b.format = SCANMSG_FORMAT_RGB565;
  xQueueSend(g_scanQ, &b, portMAX_DELAY);
}

/* Gwenesis push scanline */
extern "C" void IRAM_ATTR GWENESIS_PUSH_SCANLINE(int line, const uint16_t* src16, int w) {
  if (!g_scanQ) return;

  ScanMsg m;
  m.type = MSG_SCANLINE;
  m.line = (uint16_t)line;
  m.w    = (uint16_t)w;
  m.format = SCANMSG_FORMAT_RGB565;

  int srcH = (g_srcH_cached > 0) ? g_srcH_cached
                                 : (int)(screen_height ? screen_height : 224);
  m.srcH = (uint16_t)srcH;

  int copyW = (w < FB_W) ? w : FB_W;
#if MD_DISPLAY_SCANLINE_RING
  uint8_t slot = 0;
  if (!md_display_take_scan_slot(&slot)) return;
  ScanLineSlot *scan = &s_scanSlots[slot];
  scan->format = SCANMSG_FORMAT_RGB565;
  memcpy(scan->data, src16, copyW * sizeof(uint16_t));
  if (copyW < FB_W) memset(scan->data + copyW, 0, (FB_W - copyW) * sizeof(uint16_t));
  m.slot = slot;
#else
  // Copie ligne
  memcpy(m.data, src16, copyW * sizeof(uint16_t));
  if (copyW < FB_W) memset(m.data + copyW, 0, (FB_W - copyW) * sizeof(uint16_t));
#endif

#if MD_RENDER_LOGS_ENABLED
  const BaseType_t ok = md_display_send_scan_msg(g_scanQ, &m, portMAX_DELAY);
#else
  const BaseType_t ok = xQueueSend(g_scanQ, &m, portMAX_DELAY);
#endif
#if MD_DISPLAY_SCANLINE_RING
  if (ok != pdTRUE) {
    md_display_release_scan_slot(slot);
  }
#endif
}

/* Gwenesis push indexed scanline. The palette is snapshotted per line so the
 * display task can lag safely behind VDP CRAM updates. */
extern "C" void IRAM_ATTR GWENESIS_PUSH_SCANLINE_IDX(int line, const uint8_t* src8, int w) {
  if (!g_scanQ) return;

  ScanMsg m;
  m.type = MSG_SCANLINE;
  m.line = (uint16_t)line;
  m.w    = (uint16_t)w;
  m.format = SCANMSG_FORMAT_INDEX8;

  int srcH = (g_srcH_cached > 0) ? g_srcH_cached
                                 : (int)(screen_height ? screen_height : 224);
  m.srcH = (uint16_t)srcH;

  int copyW = (w < FB_W) ? w : FB_W;
#if MD_DISPLAY_SCANLINE_RING
  uint8_t slot = 0;
  if (!md_display_take_scan_slot(&slot)) return;
  ScanLineSlot *scan = &s_scanSlots[slot];
  scan->format = SCANMSG_FORMAT_INDEX8;
  memcpy(scan->indexed.idx, src8, copyW);
  if (copyW < FB_W) memset(scan->indexed.idx + copyW, 0, FB_W - copyW);
  if (CRAM565) {
    memcpy(scan->indexed.palette, CRAM565, sizeof(scan->indexed.palette));
  } else {
    memset(scan->indexed.palette, 0, sizeof(scan->indexed.palette));
  }
  m.slot = slot;
#else
  memcpy(m.indexed.idx, src8, copyW);
  if (copyW < FB_W) memset(m.indexed.idx + copyW, 0, FB_W - copyW);
  if (CRAM565) {
    memcpy(m.indexed.palette, CRAM565, sizeof(m.indexed.palette));
  } else {
    memset(m.indexed.palette, 0, sizeof(m.indexed.palette));
  }
#endif

#if MD_RENDER_LOGS_ENABLED
  const BaseType_t ok = md_display_send_scan_msg(g_scanQ, &m, portMAX_DELAY);
#else
  const BaseType_t ok = xQueueSend(g_scanQ, &m, portMAX_DELAY);
#endif
#if MD_DISPLAY_SCANLINE_RING
  if (ok != pdTRUE) {
    md_display_release_scan_slot(slot);
  }
#endif
}

/* End the current frame */
extern "C" void genesis_display_end_frame(void) {
  if (!g_scanQ) return;
  uint16_t h = (uint16_t)(screen_height ? screen_height : 224u);
  ScanMsg e = {};
  e.type = MSG_END_FRAME;
  e.w = (uint16_t)FB_W;
  e.srcH = h;
  e.format = SCANMSG_FORMAT_RGB565;
  xQueueSend(g_scanQ, &e, portMAX_DELAY);
}
