// ngc_display.cpp — Dual-target display (Internal M5.Lcd / External TFT_eSPI)

#include "ngc_display.h"
#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include "../tft_setup.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include <string.h>
#include <atomic>
#include <string>

// External TFT instance
static TFT_eSPI s_tft;
static constexpr int EXT_W = 320;
static constexpr int EXT_H = 240;
static bool s_ext    = false;
static bool s_12bit  = false;

// Internal display constants
static constexpr int INT_W = 240;
static constexpr int INT_H = 135;

// Dynamic target dimensions
static int s_panelW = INT_W;
static int s_panelH = INT_H;

// Framebuffer NGPC 160x152
int ngpZoomPercent = 100;
bool ngpFullscreen =  true;
volatile unsigned g_frame_ready = 0;
volatile unsigned g_frame_counter = 0;
bool s_lastScreenMode = ngpFullscreen;

// LUT state
static bool s_lut_ready = false;

// Entrelacement odd/even
bool s_interlace_parity = false;

// Color conversion LUT
extern uint16_t* totalpalette;

static uint16_t* s_linebuf_panel = nullptr;
static uint16_t* s_lut_x_full    = nullptr;
static uint16_t* s_lut_y_full    = nullptr;
static uint16_t* s_lut_x_4x3     = nullptr;
static uint16_t* s_fb            = nullptr;

// Bottom cache (tearing reduction — snapshot bottom of drawBuffer)
static uint16_t* s_bottomCache       = nullptr;
static int       s_bottomCacheLines  = 0;
static int       s_bottomCacheW      = 0;
static int       s_bottomCacheStartY = 0;
#ifdef NGP_ONLY_RENDER_VISIBLE_LINES
uint8_t* s_lut_y_render    = nullptr;
#endif
unsigned short *drawBuffer = s_fb;

static inline const uint16_t* ngp_get_src_line(int srcY);

extern "C" void ngc_display_init(void)
{
  s_ext   = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
  s_12bit = s_ext && (g_emu_color_depth == EMU_COLOR_12BIT);

  if (s_ext) {
    s_tft.begin();
    s_tft.setRotation(3);
    s_tft.fillScreen(TFT_BLACK);
    if (s_12bit) {
      s_tft.startWrite();
      s_tft.writecommand(0x3A);
      s_tft.writedata(0x53);
      s_tft.endWrite();
    }
    s_panelW = EXT_W;
    s_panelH = EXT_H;
  } else {
    s_panelW = INT_W;
    s_panelH = INT_H;
  }

  const size_t fbBytes = (size_t)NGPC_W * (size_t)NGPC_H * sizeof(uint16_t);

  // --- Framebuffer ---
  if (!s_fb) {
    s_fb = (uint16_t*)heap_caps_malloc(fbBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fb) s_fb = (uint16_t*)malloc(fbBytes);
    if (s_fb) memset(s_fb, 0, fbBytes);
    drawBuffer = s_fb;
  }

  // --- Dynamic buffers ---
  free(s_linebuf_panel);
  s_linebuf_panel = (uint16_t*)heap_caps_malloc(s_panelW * sizeof(uint16_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  free(s_lut_x_full);
  s_lut_x_full = (uint16_t*)heap_caps_malloc(s_panelW * sizeof(uint16_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  free(s_lut_y_full);
  s_lut_y_full = (uint16_t*)heap_caps_malloc(s_panelH * sizeof(uint16_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  // 4:3 LUT: outW = max(panelW, (panelH*4)/3)
  int outW_4x3 = (s_panelH * 4) / 3;
  if (outW_4x3 > s_panelW) outW_4x3 = s_panelW;
  free(s_lut_x_4x3);
  s_lut_x_4x3 = (uint16_t*)heap_caps_malloc(outW_4x3 * sizeof(uint16_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

#ifdef NGP_ONLY_RENDER_VISIBLE_LINES
  free(s_lut_y_render);
  s_lut_y_render = (uint8_t*)heap_caps_malloc(NGPC_H * sizeof(uint8_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif

  if (s_ext) {
    s_tft.fillScreen(TFT_BLACK);
  } else {
    M5.Display.setSwapBytes(true);
    M5.Display.fillScreen(TFT_BLACK);
  }

  s_lut_ready = false;
}

extern "C" void ngc_display_set_parity(bool odd) {
  s_interlace_parity = odd;
}

static void build_scale_luts()
{
  const int panelW = s_panelW;
  const int panelH = s_panelH;
  const int srcW   = NGPC_W;
  const int srcH   = NGPC_H;

  // Fullscreen LUT
  for (int x = 0; x < panelW; ++x) {
    uint32_t sx = ((uint32_t)x * (uint32_t)srcW) / (uint32_t)panelW;
    s_lut_x_full[x] = (uint16_t)((sx < (uint32_t)srcW ? sx : (uint32_t)(srcW-1)) << 1);
  }
  for (int y = 0; y < panelH; ++y) {
    uint32_t sy = ((uint32_t)y * (uint32_t)srcH) / (uint32_t)panelH;
    s_lut_y_full[y] = (uint16_t)((sy < (uint32_t)srcH ? sy : (uint32_t)(srcH-1)));
  }

  // 4:3 aspect ratio
  const int outH   = panelH;
  int outW   = (outH * 4) / 3;
  if (outW > panelW) outW = panelW;
  const int virtW  = (srcH * 4) / 3;
  const int virtOff= (virtW - srcW) / 2;

  for (int x = 0; x < outW; ++x) {
    int virtX = ((int32_t)x * virtW) / outW;
    int srcX  = virtX - virtOff;
    s_lut_x_4x3[x] = (unsigned)srcX < (unsigned)srcW ? (uint16_t)srcX : 0;
  }

#ifdef NGP_ONLY_RENDER_VISIBLE_LINES
  for (int y = 0; y < srcH; y++)
    s_lut_y_render[y]=0;
  for (int y = 0; y < panelH; y++)
    s_lut_y_render[s_lut_y_full[y]]=1;
#endif

  s_lut_ready = true;
}

// ---------- 12-bit RGB444 helper (in-place) ----------
static inline void convert_line_12bit(uint16_t* lineBuf, int pixelCount)
{
  uint8_t *buf12 = (uint8_t*)lineBuf;
  int pairs = pixelCount / 2;
  for (int p = 0; p < pairs; p++) {
    uint16_t c1 = lineBuf[p * 2];
    uint16_t c2 = lineBuf[p * 2 + 1];
    int j = p * 3;
    buf12[j]   = ((c1 >> 8) & 0xF0) | ((c1 >> 7) & 0x0F);
    buf12[j+1] = ((c1 << 3) & 0xF0) | ((c2 >> 12) & 0x0F);
    buf12[j+2] = ((c2 >> 3) & 0xF0) | ((c2 >> 1) & 0x0F);
  }
  if (pixelCount & 1) {
    uint16_t c = lineBuf[pixelCount - 1];
    int j = pairs * 3;
    buf12[j]   = ((c >> 8) & 0xF0) | ((c >> 7) & 0x0F);
    buf12[j+1] = ((c << 3) & 0xF0);
    buf12[j+2] = 0;
  }
}

static inline void push_line_ext(uint16_t* lineBuf, int pixelCount)
{
  if (s_12bit) {
    convert_line_12bit(lineBuf, pixelCount);
    int byteCount = ((pixelCount + 1) / 2) * 3;
    s_tft.pushColors((uint16_t*)lineBuf, (byteCount + 1) / 2, false);
  } else {
    s_tft.pushColors(lineBuf, pixelCount, true);
  }
}

static inline void paint_fullscreen_stretch()
{
  const int panelW = s_panelW;
  const int panelH = s_panelH;
  const int srcW   = NGPC_W;

  if (s_ext) {
    s_tft.startWrite();
    // Re-assert COLMOD every frame (safety net — s_tft.begin() elsewhere resets to 16-bit)
    if (s_12bit) {
      s_tft.writecommand(0x3A);
      s_tft.writedata(0x53);
    }
  } else {
    M5.Display.startWrite();
  }

  #ifdef NGP_INTERLACED
  const int parity = (int)s_interlace_parity;
  for (int y = parity; y < panelH; y += 2) {
  #else
  for (int y = 0; y < panelH; y += 1) {
  #endif
    const uint16_t  srcY    = s_lut_y_full[y];
    const uint16_t* srcLine = ngp_get_src_line(srcY);
    const uint8_t*  base    = (const uint8_t*)srcLine;
    const uint16_t* lutx    = s_lut_x_full;
    uint16_t*       dst     = s_linebuf_panel;

    int x = 0;
    for (; x <= panelW - 8; x += 8) {
  #ifdef  NGP_USE_THREADED_COLORMAPPING
      dst[x+0] = totalpalette[*(const uint16_t*)(base + lutx[x+0])];
      dst[x+1] = totalpalette[*(const uint16_t*)(base + lutx[x+1])];
      dst[x+2] = totalpalette[*(const uint16_t*)(base + lutx[x+2])];
      dst[x+3] = totalpalette[*(const uint16_t*)(base + lutx[x+3])];
      dst[x+4] = totalpalette[*(const uint16_t*)(base + lutx[x+4])];
      dst[x+5] = totalpalette[*(const uint16_t*)(base + lutx[x+5])];
      dst[x+6] = totalpalette[*(const uint16_t*)(base + lutx[x+6])];
      dst[x+7] = totalpalette[*(const uint16_t*)(base + lutx[x+7])];
  #else
      dst[x+0] = *(const uint16_t*)(base + lutx[x+0]);
      dst[x+1] = *(const uint16_t*)(base + lutx[x+1]);
      dst[x+2] = *(const uint16_t*)(base + lutx[x+2]);
      dst[x+3] = *(const uint16_t*)(base + lutx[x+3]);
      dst[x+4] = *(const uint16_t*)(base + lutx[x+4]);
      dst[x+5] = *(const uint16_t*)(base + lutx[x+5]);
      dst[x+6] = *(const uint16_t*)(base + lutx[x+6]);
      dst[x+7] = *(const uint16_t*)(base + lutx[x+7]);
#endif
    }
    for (; x < panelW; ++x) {
#ifdef NGP_USE_THREADED_COLORMAPPING
      dst[x] = totalpalette[*(const uint16_t*)(base + lutx[x])];
#else
      dst[x] = *(const uint16_t*)(base + lutx[x]);
#endif
    }

    if (s_ext) {
      // Per-line setAddrWindow: required because interlace skips lines
      s_tft.setAddrWindow(0, y, panelW, 1);
      push_line_ext(dst, panelW);
    } else {
      M5.Display.setAddrWindow(0, y, panelW, 1);
      M5.Display.writePixels(dst, panelW, /*swap=*/true);
    }
  }

  if (s_ext) {
    s_tft.endWrite();
  } else {
    M5.Display.endWrite();
  }
}

static inline void paint_fullheight_4x3()
{
  const int panelW = s_panelW;
  const int panelH = s_panelH;
  const int outW    = NGPC_W;  // 160 — original width
  const int x_start = (panelW - outW) / 2;

  if (s_ext) {
    s_tft.startWrite();
    // Re-assert COLMOD every frame (safety net — s_tft.begin() elsewhere resets to 16-bit)
    if (s_12bit) {
      s_tft.writecommand(0x3A);
      s_tft.writedata(0x53);
    }
  } else {
    M5.Display.startWrite();
  }

  #ifdef NGP_INTERLACED
  const int parity = (int)s_interlace_parity;
  for (int y = parity; y < panelH; y += 2) {
  #else
  for (int y = 0; y < panelH; y += 1) {
  #endif
    const uint16_t  srcY    = s_lut_y_full[y];
    const uint16_t* srcLine = ngp_get_src_line(srcY);
    const uint8_t*  base    = (const uint8_t*)srcLine;
    uint16_t*       dst     = s_linebuf_panel;

    int x = 0;
    for (; x <= 160 - 8; x += 8) {
  #ifdef  NGP_USE_THREADED_COLORMAPPING
      dst[x+0] = totalpalette[*(const uint16_t*)(base + ((x+0) << 1))];
      dst[x+1] = totalpalette[*(const uint16_t*)(base + ((x+1) << 1))];
      dst[x+2] = totalpalette[*(const uint16_t*)(base + ((x+2) << 1))];
      dst[x+3] = totalpalette[*(const uint16_t*)(base + ((x+3) << 1))];
      dst[x+4] = totalpalette[*(const uint16_t*)(base + ((x+4) << 1))];
      dst[x+5] = totalpalette[*(const uint16_t*)(base + ((x+5) << 1))];
      dst[x+6] = totalpalette[*(const uint16_t*)(base + ((x+6) << 1))];
      dst[x+7] = totalpalette[*(const uint16_t*)(base + ((x+7) << 1))];
  #else
      dst[x+0] = *(const uint16_t*)(base + ((x+0) << 1));
      dst[x+1] = *(const uint16_t*)(base + ((x+1) << 1));
      dst[x+2] = *(const uint16_t*)(base + ((x+2) << 1));
      dst[x+3] = *(const uint16_t*)(base + ((x+3) << 1));
      dst[x+4] = *(const uint16_t*)(base + ((x+4) << 1));
      dst[x+5] = *(const uint16_t*)(base + ((x+5) << 1));
      dst[x+6] = *(const uint16_t*)(base + ((x+6) << 1));
      dst[x+7] = *(const uint16_t*)(base + ((x+7) << 1));
#endif
    }
    for (; x < 160; ++x) {
#ifdef NGP_USE_THREADED_COLORMAPPING
      dst[x] = totalpalette[*(const uint16_t*)(base + (x << 1))];
#else
      dst[x] = *(const uint16_t*)(base + (x << 1));
#endif
    }

    if (s_ext) {
      // Per-line setAddrWindow: required because interlace skips lines
      s_tft.setAddrWindow(x_start, y, outW, 1);
      push_line_ext(dst, outW);
    } else {
      M5.Display.setAddrWindow(x_start, y, outW, 1);
      M5.Display.writePixels(dst, outW, /*swap=*/true);
    }
  }

  if (s_ext) {
    s_tft.endWrite();
  } else {
    M5.Display.endWrite();
  }
}

// Snapshot bottom of drawBuffer for tearing reduction
static void ngp_snapshot_bottom_cache()
{
  const int srcH = NGPC_H;
  const int srcW = NGPC_W;
  int cacheLines = (srcH * 60 + 99) / 100;
  if (cacheLines < 1) cacheLines = 1;
  int cacheStartY = srcH - cacheLines;
  if (cacheStartY < 0) cacheStartY = 0;
  int neededWords = cacheLines * srcW;

  if (!s_bottomCache || neededWords > (s_bottomCacheLines * s_bottomCacheW)) {
    free(s_bottomCache);
    s_bottomCache = (uint16_t*)heap_caps_malloc(
        neededWords * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (s_bottomCache) {
      s_bottomCacheLines = cacheLines;
      s_bottomCacheW     = srcW;
    }
  }
  s_bottomCacheStartY = cacheStartY;

  if (s_bottomCache) {
    for (int i = 0; i < cacheLines; ++i) {
      int ySrc = cacheStartY + i;
      if (ySrc >= srcH) break;
      memcpy(s_bottomCache + i * srcW,
             drawBuffer + ySrc * srcW,
             srcW * sizeof(uint16_t));
    }
  }
}

// Get source line, using cache for bottom lines
static inline const uint16_t* ngp_get_src_line(int srcY)
{
  if (s_bottomCache && srcY >= s_bottomCacheStartY) {
    int idx = srcY - s_bottomCacheStartY;
    if (idx >= 0 && idx < s_bottomCacheLines) {
      return s_bottomCache + idx * s_bottomCacheW;
    }
  }
  return drawBuffer + (size_t)srcY * NGPC_W;
}

extern "C" IRAM_ATTR void graphics_paint(unsigned char render)
{
  if (!render || !drawBuffer) return;
  if (!s_lut_ready) build_scale_luts();

  // Snapshot bottom of framebuffer before rendering (tearing reduction)
  ngp_snapshot_bottom_cache();

  if (s_lastScreenMode && !ngpFullscreen) {
    if (s_ext) {
      s_tft.fillScreen(TFT_BLACK);
    } else {
      M5.Display.fillScreen(TFT_BLACK);
    }
  }

  s_lastScreenMode = ngpFullscreen;

  if (ngpFullscreen) {
    paint_fullscreen_stretch();
  } else {
    paint_fullheight_4x3();
  }

#ifndef NGP_HW_INTERLACED
  s_interlace_parity = !s_interlace_parity;
#endif
}

// ================== EXTERNAL INFO SCREEN ==================

static std::string ngp_truncate(const char* text, size_t maxChars)
{
  if (!text) return "";
  std::string v(text);
  if (v.size() <= maxChars) return v;
  if (maxChars <= 3) return v.substr(0, maxChars);
  return v.substr(0, maxChars - 3) + "...";
}

static void ngp_draw_key_badge(int x, int y, const std::string& key)
{
  const int bw = 34, bh = 18;
  s_tft.fillRoundRect(x, y, bw, bh, 4, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  s_tft.drawCentreString(key.c_str(), x + bw / 2, y + 4, 2);
  s_tft.drawRoundRect(x, y, bw, bh, 4, TFT_YELLOW);
}

void ngc_display_show_external_info(const char* romTitle)
{
  s_tft.begin();
  s_tft.setRotation(3);
  s_tft.fillScreen(TFT_BLACK);
  s_tft.setTextWrap(false);

  s_tft.drawRoundRect(8, 8, EXT_W - 16, EXT_H - 16, 8, TFT_DARKGREY);

  std::string title = romTitle ? romTitle : "";
  const char* drawTitle = title.empty() ? "NEO GEO POCKET" : title.c_str();
  int titleFont = 4;
  int maxTitleW = EXT_W - 32;
  if (s_tft.textWidth(drawTitle, 4) > maxTitleW) {
    titleFont = 2;
    if (s_tft.textWidth(drawTitle, 2) > maxTitleW) {
      title = ngp_truncate(romTitle, 36);
      drawTitle = title.c_str();
    }
  }
  s_tft.setTextColor(TFT_CYAN, TFT_BLACK);
  s_tft.drawCentreString(drawTitle, EXT_W / 2, 16, titleFont);

  s_tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  s_tft.drawCentreString("SNK NEO GEO POCKET COLOR", EXT_W / 2, 46, 2);

  s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
  s_tft.drawCentreString("VIDEO ON INTERNAL LCD", EXT_W / 2, 64, 2);

  s_tft.drawRoundRect(12, 86, EXT_W - 24, 98, 6, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft.drawCentreString("CONTROLS", EXT_W / 2, 92, 2);

  const auto actions = share::emuControlActionLabels(share::EmuProfile::Ngp);
  const auto keys    = share::emuControlKeyLabels(share::EmuProfile::Ngp);
  const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
  const size_t rowsPerCol = 4;

  for (size_t i = 0; i < count; ++i) {
    const int col = (int)(i / rowsPerCol);
    const int row = (int)(i % rowsPerCol);
    const int baseX = 24 + col * 146;
    const int baseY = 112 + row * 16;

    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawString(actions[i].c_str(), baseX, baseY, 2);
    ngp_draw_key_badge(baseX + 88, baseY - 3, keys[i]);
  }

  s_tft.drawFastHLine(18, 190, EXT_W - 36, TFT_DARKGREY);
  s_tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  s_tft.drawString("GO = QUIT", 18, 198, 1);
  s_tft.drawString("HOLD GO = CONFIG", 100, 198, 1);
  s_tft.drawString("\\ = SCREEN", 18, 210, 1);
}
