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
static portMUX_TYPE s_overlayMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_overlayFpsEnabled = false;
static float s_overlayFps = 0.0f;
static char s_overlayFpsText[8] = "0.0";
static bool s_overlayMenuVisible = false;
static int s_overlayMenuSelected = 0;
static char s_overlayMenuTitle[24] = "VIDEO MENU";
static char s_overlayMenuRow0Label[16] = "FPS";
static char s_overlayMenuRow0Value[32] = "Off";
static char s_overlayMenuRow1Label[16] = "FRAMESKIP";
static char s_overlayMenuRow1Value[32] = "Off";
static char s_overlayMenuRow2Label[16] = "WallClk";
static char s_overlayMenuRow2Value[32] = "Off";
static char s_overlayMenuHint1[32] = "START enter < > change";
static char s_overlayMenuHint2[32] = "GO close menu";

static constexpr uint16_t kMdUiBackground = TFT_BLACK;
static constexpr uint16_t kMdUiPrimary = 0xFC20;
static constexpr uint16_t kMdUiRectDark = 0x0841;
static constexpr uint16_t kMdUiText = 0xEF7D;
static constexpr int kMdRuntimeMenuBoxW = 168;
static constexpr int kMdRuntimeMenuBoxH = 104;
static constexpr int kMdRuntimeMenuInnerPad = 8;
static constexpr int kMdRuntimeMenuRowH = 13;
static constexpr int kMdRuntimeMenuShadowOffset = 4;
static constexpr int kMdFpsHudMarginX = 4;
static constexpr int kMdFpsHudMarginY = 4;
static constexpr int kMdFpsHudPadX = 2;
static constexpr int kMdFpsHudPadY = 2;
static constexpr int kMdFpsHudScale = 2;
static constexpr int kMdFpsHudGlyphW = 3;
static constexpr int kMdFpsHudGlyphH = 5;

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

static inline void copy_overlay_text(char* dst, size_t dstSize, const char* src)
{
  if (!dst || dstSize == 0) return;
  if (!src) src = "";
  snprintf(dst, dstSize, "%s", src);
}

static int md_display_runtime_menu_box_x()
{
  return (g_dstW - kMdRuntimeMenuBoxW) / 2;
}

static int md_display_runtime_menu_box_y()
{
  return 22;
}

static int md_display_runtime_menu_first_row_y()
{
  return md_display_runtime_menu_box_y() + 22;
}

static void md_display_draw_runtime_menu_shell(const char* title,
                                               const char* hint1,
                                               const char* hint2)
{
  const int boxX = md_display_runtime_menu_box_x();
  const int boxY = md_display_runtime_menu_box_y();
  const int innerX = boxX + kMdRuntimeMenuInnerPad;
  if (!title) title = "VIDEO MENU";
  if (!hint1) hint1 = "";
  if (!hint2) hint2 = "";

  M5.Lcd.fillRect(boxX + kMdRuntimeMenuShadowOffset,
                  boxY + kMdRuntimeMenuShadowOffset,
                  kMdRuntimeMenuBoxW,
                  kMdRuntimeMenuBoxH,
                  TFT_BLACK);
  M5.Lcd.fillRect(boxX, boxY, kMdRuntimeMenuBoxW, kMdRuntimeMenuBoxH, TFT_BLACK);
  M5.Lcd.drawRect(boxX, boxY, kMdRuntimeMenuBoxW, kMdRuntimeMenuBoxH, kMdUiPrimary);
  M5.Lcd.drawRect(boxX + 2, boxY + 2, kMdRuntimeMenuBoxW - 4, kMdRuntimeMenuBoxH - 4, kMdUiRectDark);

  M5.Lcd.setTextDatum(top_left);
  M5.Lcd.setFont(&fonts::Font2);
  M5.Lcd.setTextColor(kMdUiPrimary, TFT_BLACK);
  const int titleX = boxX + (kMdRuntimeMenuBoxW - M5.Lcd.textWidth(title)) / 2;
  M5.Lcd.drawString(title, titleX, boxY + 4);
  M5.Lcd.setFont(&fonts::Font0);
  M5.Lcd.setTextColor(kMdUiText, TFT_BLACK);
  M5.Lcd.drawString(hint1, innerX, boxY + kMdRuntimeMenuBoxH - 19);
  M5.Lcd.drawString(hint2, innerX, boxY + kMdRuntimeMenuBoxH - 10);
}

static void md_display_draw_runtime_menu_row(int row,
                                             bool selected,
                                             const char* label,
                                             const char* value)
{
  const int boxX = md_display_runtime_menu_box_x();
  const int innerX = boxX + kMdRuntimeMenuInnerPad;
  const int valueX = boxX + kMdRuntimeMenuBoxW - 58;
  const int y = md_display_runtime_menu_first_row_y() + row * kMdRuntimeMenuRowH;
  const int rowX = innerX - 4;
  const int rowY = y - 2;
  const int rowW = kMdRuntimeMenuBoxW - 16;
  const int rowH = kMdRuntimeMenuRowH - 1;
  const int valueClearW = (boxX + kMdRuntimeMenuBoxW - 4) - valueX;
  const uint16_t rowBg = selected ? kMdUiRectDark : TFT_BLACK;

  M5.Lcd.fillRect(rowX, rowY, rowW, rowH, rowBg);
  M5.Lcd.setFont(&fonts::Font0);
  M5.Lcd.setTextColor(selected ? kMdUiPrimary : kMdUiText, rowBg);
  M5.Lcd.drawString(label ? label : "", innerX, y);
  if (value) {
    M5.Lcd.fillRect(valueX, rowY, valueClearW, rowH, rowBg);
    M5.Lcd.drawString(value, valueX, y);
  }
}

static void md_display_draw_menu_overlay(int selectedRow,
                                         const char* title,
                                         const char* row0Label,
                                         const char* row0Value,
                                         const char* row1Label,
                                         const char* row1Value,
                                         const char* row2Label,
                                         const char* row2Value,
                                         const char* hint1,
                                         const char* hint2)
{
  md_display_draw_runtime_menu_shell(title, hint1, hint2);
  md_display_draw_runtime_menu_row(0, selectedRow == 0, row0Label, row0Value);
  md_display_draw_runtime_menu_row(1, selectedRow == 1, row1Label, row1Value);
  if ((row2Label && row2Label[0]) || (row2Value && row2Value[0])) {
    md_display_draw_runtime_menu_row(2, selectedRow == 2, row2Label, row2Value);
  }
}

static uint8_t md_display_fps_glyph_row(char ch, int row)
{
  if (row < 0 || row >= kMdFpsHudGlyphH) return 0u;
  switch (ch) {
    case '0': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x07u, 0x05u, 0x05u, 0x05u, 0x07u}; return r[row]; }
    case '1': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x02u, 0x06u, 0x02u, 0x02u, 0x07u}; return r[row]; }
    case '2': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x07u, 0x01u, 0x07u, 0x04u, 0x07u}; return r[row]; }
    case '3': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x07u, 0x01u, 0x07u, 0x01u, 0x07u}; return r[row]; }
    case '4': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x05u, 0x05u, 0x07u, 0x01u, 0x01u}; return r[row]; }
    case '5': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x07u, 0x04u, 0x07u, 0x01u, 0x07u}; return r[row]; }
    case '6': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x07u, 0x04u, 0x07u, 0x05u, 0x07u}; return r[row]; }
    case '7': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x07u, 0x01u, 0x01u, 0x01u, 0x01u}; return r[row]; }
    case '8': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x07u, 0x05u, 0x07u, 0x05u, 0x07u}; return r[row]; }
    case '9': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x07u, 0x05u, 0x07u, 0x01u, 0x07u}; return r[row]; }
    case '.': { static constexpr uint8_t r[kMdFpsHudGlyphH] = {0x00u, 0x00u, 0x00u, 0x00u, 0x02u}; return r[row]; }
    case ' ':
    default:
      return 0u;
  }
}

static int md_display_fps_hud_box_width(const char* text)
{
  const size_t len = text ? strlen(text) : 0;
  if (len == 0) return 0;
  const int advance = (kMdFpsHudGlyphW + 1) * kMdFpsHudScale;
  const int textW = (int)len * advance - kMdFpsHudScale;
  return (kMdFpsHudPadX * 2) + textW;
}

static int md_display_fps_hud_box_height()
{
  return (kMdFpsHudPadY * 2) + (kMdFpsHudGlyphH * kMdFpsHudScale);
}

static bool md_display_get_fps_hud_text(char* dst, size_t dstSize)
{
  if (!dst || dstSize == 0) return false;
  bool draw = false;
  taskENTER_CRITICAL(&s_overlayMux);
  draw = s_overlayFpsEnabled && !s_overlayMenuVisible;
  copy_overlay_text(dst, dstSize, s_overlayFpsText);
  taskEXIT_CRITICAL(&s_overlayMux);
  return draw && dst[0] != '\0';
}

static bool md_display_fps_hud_bounds(const char* text, int* boxX, int* boxY, int* boxW, int* boxH)
{
  const int w = md_display_fps_hud_box_width(text);
  const int h = md_display_fps_hud_box_height();
  if (w <= 0 || h <= 0 || w > g_dstW || h > g_dstH) return false;
  if (boxX) *boxX = (g_dstW > w + kMdFpsHudMarginX) ? (g_dstW - w - kMdFpsHudMarginX) : 0;
  if (boxY) *boxY = kMdFpsHudMarginY;
  if (boxW) *boxW = w;
  if (boxH) *boxH = h;
  return true;
}

static void md_display_draw_fps_hud_row(uint16_t* dst, int dstW, int dstY, const char* text)
{
  int boxX = 0;
  int boxY = 0;
  int boxW = 0;
  int boxH = 0;
  if (!dst || !md_display_fps_hud_bounds(text, &boxX, &boxY, &boxW, &boxH)) return;
  if (dstY < boxY || dstY >= boxY + boxH) return;

  const int localY = dstY - boxY;
  const int fillLimit = (boxX + boxW < dstW) ? (boxX + boxW) : dstW;
  for (int x = boxX; x < fillLimit; ++x) {
    dst[x] = 0u;
  }

  if (localY < kMdFpsHudPadY ||
      localY >= kMdFpsHudPadY + (kMdFpsHudGlyphH * kMdFpsHudScale)) {
    return;
  }

  const int advance = (kMdFpsHudGlyphW + 1) * kMdFpsHudScale;
  const int glyphRow = (localY - kMdFpsHudPadY) / kMdFpsHudScale;
  const int textX0 = boxX + kMdFpsHudPadX;
  const size_t len = strlen(text);
  for (size_t i = 0; i < len; ++i) {
    const uint8_t bits = md_display_fps_glyph_row(text[i], glyphRow);
    if (bits == 0u) continue;
    const int charX0 = textX0 + (int)i * advance;
    for (int col = 0; col < kMdFpsHudGlyphW; ++col) {
      const uint8_t mask = (uint8_t)(1u << (kMdFpsHudGlyphW - 1 - col));
      if ((bits & mask) == 0u) continue;
      for (int sx = 0; sx < kMdFpsHudScale; ++sx) {
        const int pixelX = charX0 + col * kMdFpsHudScale + sx;
        if (pixelX >= boxX && pixelX < fillLimit) {
          dst[pixelX] = TFT_WHITE;
        }
      }
    }
  }
}

static void md_display_write_line_with_fps_hud(int dstY, const char* fpsText)
{
  int boxX = 0;
  int boxY = 0;
  int boxW = 0;
  int boxH = 0;
  if (!fpsText ||
      !md_display_fps_hud_bounds(fpsText, &boxX, &boxY, &boxW, &boxH) ||
      dstY < boxY ||
      dstY >= boxY + boxH ||
      boxW > 64) {
    M5.Lcd.writePixels(s_lineFull, g_dstW);
    return;
  }

  uint16_t backup[64];
  memcpy16(backup, s_lineFull + boxX, boxW);
  md_display_draw_fps_hud_row(s_lineFull, g_dstW, dstY, fpsText);
  M5.Lcd.writePixels(s_lineFull, g_dstW);
  memcpy16(s_lineFull + boxX, backup, boxW);
}

static void md_display_write_lines(int y, int count)
{
  char fpsText[8];
  const bool drawFps = md_display_get_fps_hud_text(fpsText, sizeof(fpsText));
  int boxX = 0;
  int boxY = 0;
  int boxW = 0;
  int boxH = 0;
  const bool fpsHasBounds = drawFps && md_display_fps_hud_bounds(fpsText, &boxX, &boxY, &boxW, &boxH);
  const bool overlapsFps = fpsHasBounds && y < boxY + boxH && y + count > boxY;
  if (!overlapsFps) {
    M5.Lcd.setAddrWindow(0, y, g_dstW, count);
    for (int i = 0; i < count; ++i) {
      M5.Lcd.writePixels(s_lineFull, g_dstW);
    }
    return;
  }

  for (int i = 0; i < count; ++i) {
    M5.Lcd.setAddrWindow(0, y + i, g_dstW, 1);
    md_display_write_line_with_fps_hud(y + i, fpsText);
  }
}

static void md_display_draw_fps_hud_direct(float fps)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "%.1f", (double)fps);
  const int boxW = md_display_fps_hud_box_width(buf);
  const int boxH = md_display_fps_hud_box_height();
  if (boxW <= 0) return;
  const int boxX = (g_dstW > boxW + kMdFpsHudMarginX) ? (g_dstW - boxW - kMdFpsHudMarginX) : 0;
  const int boxY = kMdFpsHudMarginY;
  const int clearX = (g_dstW > 52) ? (g_dstW - 52) : 0;

  M5.Lcd.fillRect(clearX, boxY, g_dstW - clearX, boxH, TFT_BLACK);
  M5.Lcd.fillRect(boxX, boxY, boxW, boxH, TFT_BLACK);

  const int advance = (kMdFpsHudGlyphW + 1) * kMdFpsHudScale;
  const int textX0 = boxX + kMdFpsHudPadX;
  const int textY0 = boxY + kMdFpsHudPadY;
  const size_t len = strlen(buf);
  for (size_t i = 0; i < len; ++i) {
    const int charX0 = textX0 + (int)i * advance;
    for (int row = 0; row < kMdFpsHudGlyphH; ++row) {
      const uint8_t bits = md_display_fps_glyph_row(buf[i], row);
      for (int col = 0; col < kMdFpsHudGlyphW; ++col) {
        const uint8_t mask = (uint8_t)(1u << (kMdFpsHudGlyphW - 1 - col));
        if ((bits & mask) == 0u) continue;
        M5.Lcd.fillRect(charX0 + col * kMdFpsHudScale,
                        textY0 + row * kMdFpsHudScale,
                        kMdFpsHudScale,
                        kMdFpsHudScale,
                        TFT_WHITE);
      }
    }
  }
}

static void md_display_draw_overlay()
{
  bool fpsEnabled = false;
  float fps = 0.0f;
  bool menuVisible = false;
  int menuSelected = 0;
  char menuTitle[24];
  char menuRow0Label[16];
  char menuRow0Value[32];
  char menuRow1Label[16];
  char menuRow1Value[32];
  char menuRow2Label[16];
  char menuRow2Value[32];
  char menuHint1[32];
  char menuHint2[32];

  taskENTER_CRITICAL(&s_overlayMux);
  fpsEnabled = s_overlayFpsEnabled;
  fps = s_overlayFps;
  menuVisible = s_overlayMenuVisible;
  menuSelected = s_overlayMenuSelected;
  copy_overlay_text(menuTitle, sizeof(menuTitle), s_overlayMenuTitle);
  copy_overlay_text(menuRow0Label, sizeof(menuRow0Label), s_overlayMenuRow0Label);
  copy_overlay_text(menuRow0Value, sizeof(menuRow0Value), s_overlayMenuRow0Value);
  copy_overlay_text(menuRow1Label, sizeof(menuRow1Label), s_overlayMenuRow1Label);
  copy_overlay_text(menuRow1Value, sizeof(menuRow1Value), s_overlayMenuRow1Value);
  copy_overlay_text(menuRow2Label, sizeof(menuRow2Label), s_overlayMenuRow2Label);
  copy_overlay_text(menuRow2Value, sizeof(menuRow2Value), s_overlayMenuRow2Value);
  copy_overlay_text(menuHint1, sizeof(menuHint1), s_overlayMenuHint1);
  copy_overlay_text(menuHint2, sizeof(menuHint2), s_overlayMenuHint2);
  taskEXIT_CRITICAL(&s_overlayMux);

  if (!fpsEnabled && !menuVisible) return;

  if (fpsEnabled && !menuVisible && !g_scanQ) {
    md_display_draw_fps_hud_direct(fps);
  }

  if (!menuVisible) return;

  md_display_draw_menu_overlay(menuSelected,
                               menuTitle,
                               menuRow0Label,
                               menuRow0Value,
                               menuRow1Label,
                               menuRow1Value,
                               menuRow2Label,
                               menuRow2Value,
                               menuHint1,
                               menuHint2);
}

extern "C" void genesis_display_set_fps_overlay(bool enabled, float fps)
{
  taskENTER_CRITICAL(&s_overlayMux);
  s_overlayFpsEnabled = enabled;
  s_overlayFps = fps;
  snprintf(s_overlayFpsText, sizeof(s_overlayFpsText), "%.1f", (double)fps);
  taskEXIT_CRITICAL(&s_overlayMux);
}

extern "C" void genesis_display_set_menu_overlay(bool visible,
                                                  int selectedRow,
                                                  const char* title,
                                                  const char* row0Label,
                                                  const char* row0Value,
                                                  const char* row1Label,
                                                  const char* row1Value,
                                                  const char* row2Label,
                                                  const char* row2Value,
                                                  const char* hint1,
                                                  const char* hint2)
{
  taskENTER_CRITICAL(&s_overlayMux);
  s_overlayMenuVisible = visible;
  s_overlayMenuSelected = clampi(selectedRow, 0, 2);
  copy_overlay_text(s_overlayMenuTitle, sizeof(s_overlayMenuTitle), title);
  copy_overlay_text(s_overlayMenuRow0Label, sizeof(s_overlayMenuRow0Label), row0Label);
  copy_overlay_text(s_overlayMenuRow0Value, sizeof(s_overlayMenuRow0Value), row0Value);
  copy_overlay_text(s_overlayMenuRow1Label, sizeof(s_overlayMenuRow1Label), row1Label);
  copy_overlay_text(s_overlayMenuRow1Value, sizeof(s_overlayMenuRow1Value), row1Value);
  copy_overlay_text(s_overlayMenuRow2Label, sizeof(s_overlayMenuRow2Label), row2Label);
  copy_overlay_text(s_overlayMenuRow2Value, sizeof(s_overlayMenuRow2Value), row2Value);
  copy_overlay_text(s_overlayMenuHint1, sizeof(s_overlayMenuHint1), hint1);
  copy_overlay_text(s_overlayMenuHint2, sizeof(s_overlayMenuHint2), hint2);
  taskEXIT_CRITICAL(&s_overlayMux);
}

static void md_display_request_overlay_ticks(TickType_t ticks)
{
  if (!g_scanQ) {
    md_display_draw_overlay();
    return;
  }
  ScanMsg msg = {};
  msg.type = MSG_OVERLAY;
  xQueueSend(g_scanQ, &msg, ticks);
}

extern "C" void genesis_display_request_overlay(void)
{
  md_display_request_overlay_ticks(0);
}

extern "C" void genesis_display_request_overlay_blocking(uint32_t timeoutMs)
{
  md_display_request_overlay_ticks(pdMS_TO_TICKS(timeoutMs));
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
      md_display_draw_overlay();
      continue;
    }

    if (m.type == MSG_OVERLAY) {
      if (inFrame) {
        M5.Lcd.endWrite();
        inFrame = false;
      }
      md_display_draw_overlay();
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
      md_display_write_lines(prevDstY, chunk);
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
