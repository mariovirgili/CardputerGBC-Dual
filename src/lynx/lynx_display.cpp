// lynx_display.cpp — Dual-target display (Internal M5.Lcd / External TFT_eSPI)

#include "lynx_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include "../tft_setup.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "esp_heap_caps.h"
#include <string>

// External TFT instance (file-scoped)
static TFT_eSPI s_tft;
static constexpr int EXT_W = 320;
static constexpr int EXT_H = 240;
static bool s_use_ext   = false;
static bool s_use_12bit = false;

// Public
bool lynxFullScreen   = true;
int  lynxZoomPercent  = 100;

// Message
typedef struct {
  const uint16_t *fb;
  int width;
  int height;
} LynxFrameMsg;

// Globals
static QueueHandle_t s_frameQ  = nullptr;
static TaskHandle_t  s_task    = nullptr;
static uint16_t     *s_lineBuf = nullptr;
static int           s_lineCap = 0;

// Full-frame cache (tearing elimination — snapshot entire framebuffer)
static uint16_t* s_frameCache   = nullptr;
static int       s_frameCacheW  = 0;
static int       s_frameCacheH  = 0;

// Integer scaling LUTs (replaces per-pixel float math)
static int16_t  *s_xmap    = nullptr;
static int16_t  *s_ymap    = nullptr;
static int       s_xmapCap = 0;
static int       s_ymapCap = 0;

struct LynxDisplayTransform {
  int   dstW;
  int   dstH;
  int   xOffset;
  int   yOffset;
  float invScaleX;
  float invScaleY;
  float srcCX;
  float srcCY;
  float dstCX;
  float dstCY;
};

static LynxDisplayTransform s_transform;

// cache for transform computation
static int s_lastSrcW = 0;
static int s_lastSrcH = 0;
static int s_lastLcdW = 0;
static int s_lastLcdH = 0;
static int s_lastZoomPercent = -1;
static bool s_lastFullScreen  = false;

static void lynx_display_compute_transform(int srcW, int srcH)
{
  int lcdW, lcdH;
  if (s_use_ext) {
    lcdW = EXT_W;
    lcdH = EXT_H;
  } else {
    lcdW = M5Cardputer.Display.width();
    lcdH = M5Cardputer.Display.height();
  }

  if (lcdW <= 0 || lcdH <= 0 || srcW <= 0 || srcH <= 0) {
    return;
  }

  bool full = lynxFullScreen;
  int  zoom = (lynxZoomPercent > 0) ? lynxZoomPercent : 100;

  // nothing changed?
  if (zoom   == s_lastZoomPercent &&
      full   == s_lastFullScreen  &&
      srcW   == s_lastSrcW        &&
      srcH   == s_lastSrcH        &&
      lcdW   == s_lastLcdW        &&
      lcdH   == s_lastLcdH) {
    return;
  }

  // update cache
  s_lastZoomPercent = zoom;
  s_lastFullScreen  = full;
  s_lastSrcW        = srcW;
  s_lastSrcH        = srcH;
  s_lastLcdW        = lcdW;
  s_lastLcdH        = lcdH;

  float zoomFactor = zoom / 100.0f;
  if (zoomFactor <= 0.0f) zoomFactor = 1.0f;

  int   dstW, dstH;
  int   xOffset, yOffset;
  float baseInvScaleX, baseInvScaleY;

  if (!full) {
    // Mode original
    float scaleX = (float)lcdW / (float)srcW;
    float scaleY = (float)lcdH / (float)srcH;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale <= 0.0f) scale = 1.0f;

    dstW = (int)(srcW * scale);
    dstH = (int)(srcH * scale);
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;

    xOffset = (lcdW - dstW) / 2;
    yOffset = (lcdH - dstH) / 2;
    if (xOffset < 0) xOffset = 0;
    if (yOffset < 0) yOffset = 0;

    baseInvScaleX = (float)srcW / (float)dstW;
    baseInvScaleY = (float)srcH / (float)dstH;

    // Clear borders
    if (s_use_ext) {
      s_tft.fillScreen(TFT_BLACK);
    } else {
      M5Cardputer.Display.fillScreen(TFT_BLACK);
    }
  } else {
    // Mode fullscreen
    dstW = lcdW;
    dstH = lcdH;
    xOffset = 0;
    yOffset = 0;

    baseInvScaleX = (float)srcW / (float)dstW;
    baseInvScaleY = (float)srcH / (float)dstH;
  }

  // apply zoom
  float invScaleX = baseInvScaleX / zoomFactor;
  float invScaleY = baseInvScaleY / zoomFactor;

  s_transform.dstW      = dstW;
  s_transform.dstH      = dstH;
  s_transform.xOffset   = xOffset;
  s_transform.yOffset   = yOffset;
  s_transform.invScaleX = invScaleX;
  s_transform.invScaleY = invScaleY;

  s_transform.srcCX = (float)srcW * 0.5f;
  s_transform.srcCY = (float)srcH * 0.5f;
  s_transform.dstCX = (float)(dstW - 1) * 0.5f;
  s_transform.dstCY = (float)(dstH - 1) * 0.5f;

  // Build integer X LUT (eliminates per-pixel float math in render loop)
  if (dstW > s_xmapCap) {
    free(s_xmap);
    s_xmap = (int16_t*)malloc(dstW * sizeof(int16_t));
    s_xmapCap = s_xmap ? dstW : 0;
  }
  if (s_xmap) {
    float sCX = s_transform.srcCX;
    float dCX = s_transform.dstCX;
    for (int x = 0; x < dstW; x++) {
      float srcXf = sCX + ((float)x - dCX) * invScaleX;
      int sx = (int)srcXf;
      if (sx < 0)      sx = 0;
      if (sx >= srcW)   sx = srcW - 1;
      s_xmap[x] = (int16_t)sx;
    }
  }

  // Build integer Y LUT
  if (dstH > s_ymapCap) {
    free(s_ymap);
    s_ymap = (int16_t*)malloc(dstH * sizeof(int16_t));
    s_ymapCap = s_ymap ? dstH : 0;
  }
  if (s_ymap) {
    float sCY = s_transform.srcCY;
    float dCY = s_transform.dstCY;
    for (int y = 0; y < dstH; y++) {
      float srcYf = sCY + ((float)y - dCY) * invScaleY;
      int sy = (int)srcYf;
      if (sy < 0)      sy = 0;
      if (sy >= srcH)   sy = srcH - 1;
      s_ymap[y] = (int16_t)sy;
    }
  }
}


// ================== TASK ==================

static void lynx_display_task(void *arg)
{
  (void)arg;
  const bool ext   = s_use_ext;
  const bool use12 = s_use_12bit;

  for (;;) {
    LynxFrameMsg msg;
    if (xQueueReceive(s_frameQ, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (!msg.fb || msg.width <= 0 || msg.height <= 0) {
      continue;
    }

    int srcW = msg.width;
    int srcH = msg.height;

    lynx_display_compute_transform(srcW, srcH);

    int dstW    = s_transform.dstW;
    int dstH    = s_transform.dstH;
    int xOffset = s_transform.xOffset;
    int yOffset = s_transform.yOffset;

    if (dstW <= 0 || dstH <= 0 || !s_xmap || !s_ymap) {
      continue;
    }

    if (dstW > s_lineCap) {
      free(s_lineBuf);
      s_lineBuf = (uint16_t*)heap_caps_malloc(
          dstW * sizeof(uint16_t),
          MALLOC_CAP_DMA | MALLOC_CAP_8BIT
      );
      s_lineCap = s_lineBuf ? dstW : 0;
    }
    if (!s_lineBuf) {
      continue;
    }

    // Full-frame snapshot: copy entire framebuffer to eliminate tearing
    {
      int neededWords = srcW * srcH;
      if (!s_frameCache || s_frameCacheW != srcW || s_frameCacheH != srcH) {
        free(s_frameCache);
        s_frameCache = (uint16_t*)heap_caps_malloc(
            neededWords * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_frameCache)
          s_frameCache = (uint16_t*)heap_caps_malloc(
              neededWords * sizeof(uint16_t), MALLOC_CAP_8BIT);
        s_frameCacheW = s_frameCache ? srcW : 0;
        s_frameCacheH = s_frameCache ? srcH : 0;
      }
      if (s_frameCache) {
        if (use12) {
          // Convert BE → LE during copy so rendering loop reads native LE
          // (matches PCE/SMS pattern — all 12-bit paths read LE values)
          const uint16_t* src = msg.fb;
          uint16_t* dst = s_frameCache;
          for (int i = 0; i < neededWords; i++) {
            uint16_t v = src[i];
            dst[i] = (v >> 8) | (v << 8);
          }
        } else {
          memcpy(s_frameCache, msg.fb, neededWords * sizeof(uint16_t));
        }
      }
    }

    const uint16_t* fbSrc = s_frameCache ? s_frameCache : msg.fb;

    if (ext) {
      s_tft.startWrite();
      // Re-assert COLMOD every frame (safety net — s_tft.begin() elsewhere resets to 16-bit)
      if (use12) {
        s_tft.writecommand(0x3A);
        s_tft.writedata(0x53);
      }
      s_tft.setAddrWindow(xOffset, yOffset, dstW, dstH);
    } else {
      M5Cardputer.Display.startWrite();
    }

    for (int y = 0; y < dstH; ++y) {
      int srcY = s_ymap[y];
      const uint16_t* srcLine = fbSrc + srcY * srcW;

      if (ext && use12) {
        // Identical to PCE 12-bit: read LE values → pack to RGB444
        // NO in-place aliasing (reads from LE cache, writes to buf12)
        uint8_t *buf12 = (uint8_t*)s_lineBuf;
        int pairs = dstW / 2;
        for (int p = 0; p < pairs; p++) {
          uint16_t c1 = srcLine[s_xmap[p * 2]];
          uint16_t c2 = srcLine[s_xmap[p * 2 + 1]];
          int j = p * 3;
          buf12[j]   = ((c1 >> 8) & 0xF0) | ((c1 >> 7) & 0x0F);
          buf12[j+1] = ((c1 << 3) & 0xF0) | ((c2 >> 12) & 0x0F);
          buf12[j+2] = ((c2 >> 3) & 0xF0) | ((c2 >> 1) & 0x0F);
        }
        if (dstW & 1) {
          uint16_t c = srcLine[s_xmap[dstW - 1]];
          int j = pairs * 3;
          buf12[j]   = ((c >> 8) & 0xF0) | ((c >> 7) & 0x0F);
          buf12[j+1] = ((c << 3) & 0xF0);
          buf12[j+2] = 0;
        }
        int byteCount = ((dstW + 1) / 2) * 3;
        s_tft.pushColors((uint16_t*)buf12, (byteCount + 1) / 2, false);
      } else if (ext) {
        // 16-bit: scale + push (Lynx data is BE, swap=false is correct)
        for (int x = 0; x < dstW; ++x) {
          s_lineBuf[x] = srcLine[s_xmap[x]];
        }
        s_tft.pushColors(s_lineBuf, dstW, false);
      } else {
        // Internal display
        for (int x = 0; x < dstW; ++x) {
          s_lineBuf[x] = srcLine[s_xmap[x]];
        }
        int dstY = yOffset + y;
        M5Cardputer.Display.setAddrWindow(xOffset, dstY, dstW, 1);
        M5Cardputer.Display.pushPixels(s_lineBuf, dstW);
      }
    }

    if (ext) {
      s_tft.endWrite();
    } else {
      M5Cardputer.Display.endWrite();
    }
    vTaskDelay(1);
  }
}

// ================== PUBLIC API ==================

extern "C" void lynx_display_init(void)
{
  s_use_ext   = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
  s_use_12bit = s_use_ext && (g_emu_color_depth == EMU_COLOR_12BIT);

  if (s_use_ext) {
    s_tft.begin();
    s_tft.setRotation(3);
    s_tft.fillScreen(TFT_BLACK);
    if (s_use_12bit) {
      // Keep CS LOW throughout command+parameter to ensure COLMOD sticks
      s_tft.startWrite();
      s_tft.writecommand(0x3A);  // COLMOD
      s_tft.writedata(0x53);     // DPI=16bit, DBI=12bit (RGB444)
      s_tft.endWrite();
    }
  } else {
    M5Cardputer.Display.setSwapBytes(false);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
  }

  if (!s_frameQ) {
    s_frameQ = xQueueCreate(2, sizeof(LynxFrameMsg));
    if (!s_frameQ) {
      printf("[LYNX-DISP] queue create failed\n");
    }
  }
}

extern "C" void lynx_display_start(void)
{
  if (!s_frameQ) {
    lynx_display_init();
  }

  if (!s_task && s_frameQ) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      lynx_display_task,
      "LynxDisp",
      4096,
      nullptr,
      6,
      &s_task,
      0  // core 0
    );
    if (ok != pdPASS) {
      printf("[LYNX-DISP] task create failed\n");
      if (s_task) {
        vTaskDelete(s_task);
      }
      s_task = nullptr;
    }
  }
}

extern "C" void lynx_display_stop(void)
{
  if (s_task) {
    vTaskDelete(s_task);
    s_task = nullptr;
  }
  if (s_frameQ) {
    vQueueDelete(s_frameQ);
    s_frameQ = nullptr;
  }
  if (s_lineBuf) {
    free(s_lineBuf);
    s_lineBuf = nullptr;
    s_lineCap = 0;
  }
  free(s_frameCache);
  s_frameCache = nullptr;
  s_frameCacheW = 0;
  s_frameCacheH = 0;
  free(s_xmap); s_xmap = nullptr; s_xmapCap = 0;
  free(s_ymap); s_ymap = nullptr; s_ymapCap = 0;
}

extern "C" void lynx_display_submit_frame(const uint16_t *fb,
                                          int width,
                                          int height)
{
  if (!s_frameQ || !fb) return;

  LynxFrameMsg msg;
  msg.fb     = fb;
  msg.width  = width;
  msg.height = height;

  // non blocking
  BaseType_t ok = xQueueSend(s_frameQ, &msg, 0);
  (void)ok;
}

// ================== EXTERNAL INFO SCREEN ==================

static std::string lynx_truncate(const char* text, size_t maxChars)
{
  if (!text) return "";
  std::string v(text);
  if (v.size() <= maxChars) return v;
  if (maxChars <= 3) return v.substr(0, maxChars);
  return v.substr(0, maxChars - 3) + "...";
}

static void lynx_draw_key_badge(int x, int y, const std::string& key)
{
  const int bw = 34, bh = 18;
  s_tft.fillRoundRect(x, y, bw, bh, 4, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  s_tft.drawCentreString(key.c_str(), x + bw / 2, y + 4, 2);
  s_tft.drawRoundRect(x, y, bw, bh, 4, TFT_YELLOW);
}

void lynx_display_show_external_info(const char* romTitle)
{
  s_tft.begin();
  s_tft.setRotation(3);
  s_tft.fillScreen(TFT_BLACK);
  s_tft.setTextWrap(false);

  s_tft.drawRoundRect(8, 8, EXT_W - 16, EXT_H - 16, 8, TFT_DARKGREY);

  std::string title = romTitle ? romTitle : "";
  const char* drawTitle = title.empty() ? "LYNX" : title.c_str();
  int titleFont = 4;
  int maxTitleW = EXT_W - 32;
  if (s_tft.textWidth(drawTitle, 4) > maxTitleW) {
    titleFont = 2;
    if (s_tft.textWidth(drawTitle, 2) > maxTitleW) {
      title = lynx_truncate(romTitle, 36);
      drawTitle = title.c_str();
    }
  }
  s_tft.setTextColor(TFT_CYAN, TFT_BLACK);
  s_tft.drawCentreString(drawTitle, EXT_W / 2, 16, titleFont);

  s_tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  s_tft.drawCentreString("ATARI LYNX", EXT_W / 2, 46, 2);

  s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
  s_tft.drawCentreString("VIDEO ON INTERNAL LCD", EXT_W / 2, 64, 2);

  s_tft.drawRoundRect(12, 86, EXT_W - 24, 98, 6, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft.drawCentreString("CONTROLS", EXT_W / 2, 92, 2);

  const auto actions = share::emuControlActionLabels(share::EmuProfile::Lynx);
  const auto keys    = share::emuControlKeyLabels(share::EmuProfile::Lynx);
  const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
  const size_t rowsPerCol = 5;

  for (size_t i = 0; i < count; ++i) {
    const int col = (int)(i / rowsPerCol);
    const int row = (int)(i % rowsPerCol);
    const int baseX = 24 + col * 146;
    const int baseY = 112 + row * 16;

    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawString(actions[i].c_str(), baseX, baseY, 2);
    lynx_draw_key_badge(baseX + 88, baseY - 3, keys[i]);
  }

  s_tft.drawFastHLine(18, 200, EXT_W - 36, TFT_DARKGREY);
  s_tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  s_tft.drawString("GO = QUIT", 18, 208, 1);
  s_tft.drawString("HOLD GO = CONFIG", 100, 208, 1);
  s_tft.drawString("\\ = SCREEN  FN+,/ = ZOOM", 18, 220, 1);
}
