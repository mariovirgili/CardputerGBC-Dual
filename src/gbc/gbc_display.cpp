#include "gbc_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include <string>
#include <vector>

// Include the dual screen setup
#include "../tft_setup.h"
#include "../share/emu_controls.h"
#include "../share/display_target.h"
#include "esp_heap_caps.h"

extern "C" {
  #include "gnuboy/gnuboy.h"
}

// Instance for external screen
TFT_eSPI tft = TFT_eSPI();

// External TFT resolution
static constexpr int EXT_LCD_W = 320;
static constexpr int EXT_LCD_H = 240;

// Globals
bool gbcFullScreen = true;
int  gbcZoomPercent = 100;
int gbPalette = 36; // DMG default palette

// Message sent in the queue
typedef struct {
  const uint16_t *fb;
  int pitch;
  int width;
  int height;
} GbcFrameMsg;

// Globals local
static QueueHandle_t s_frameQ = nullptr;
static TaskHandle_t  s_task   = nullptr;
static uint16_t     *s_lineBuf = nullptr;
static int           s_lineCap = 0;

// Bottom cache (tearing reduction — snapshot bottom of framebuffer)
static uint16_t* s_bottomCache       = nullptr;
static int       s_bottomCacheLines  = 0;
static int       s_bottomCacheW      = 0;
static int       s_bottomCacheStartY = 0;

// Integer LUTs for scaling (precalculated, replaces per-pixel float math)
static int16_t *s_xmap = nullptr;
static int16_t *s_ymap = nullptr;
static int      s_xmapCap = 0;
static int      s_ymapCap = 0;

// Cache of transform parameters
struct GbcDisplayTransform {
  int dstW;
  int dstH;
  int xOffset;
  int yOffset;
};

static GbcDisplayTransform s_transform;
static gbc_display_target_t s_target = GBC_DISPLAY_EXTERNAL;

static int  s_lastZoomPercent = -1;
static bool s_lastFullScreen  = false;
static int  s_lastSrcW        = 0;
static int  s_lastSrcH        = 0;
static int  s_lastTargetW     = 0;
static int  s_lastTargetH     = 0;

static bool gbc_game_on_internal()
{
  return s_target == GBC_DISPLAY_INTERNAL;
}

static int gbc_target_width()
{
  return gbc_game_on_internal() ? M5Cardputer.Display.width() : EXT_LCD_W;
}

static int gbc_target_height()
{
  return gbc_game_on_internal() ? M5Cardputer.Display.height() : EXT_LCD_H;
}

static void gbc_reset_transform_cache()
{
  s_lastZoomPercent = -1;
  s_lastFullScreen  = false;
  s_lastSrcW        = 0;
  s_lastSrcH        = 0;
  s_lastTargetW     = 0;
  s_lastTargetH     = 0;
}

static void gbc_fill_active_screen_black()
{
  if (gbc_game_on_internal()) {
    M5Cardputer.Display.fillScreen(TFT_BLACK);
  } else {
    tft.fillScreen(TFT_BLACK);
  }
}

static std::string truncate_text(const char* text, size_t maxChars)
{
  if (!text) {
    return "";
  }

  std::string value(text);
  if (value.size() <= maxChars) {
    return value;
  }

  if (maxChars <= 3) {
    return value.substr(0, maxChars);
  }

  return value.substr(0, maxChars - 3) + "...";
}

static void draw_key_badge(int x, int y, const std::string& key)
{
  const int badgeW = 34;
  const int badgeH = 18;

  tft.fillRoundRect(x, y, badgeW, badgeH, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawCentreString(key.c_str(), x + badgeW / 2, y + 1, 2);
  tft.drawRoundRect(x, y, badgeW, badgeH, 4, TFT_YELLOW);
}

static void gbc_prepare_external_tft()
{
  tft.begin();
  tft.setRotation(3);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);
}

static std::string gbc_fit_title_for_width(const char* text, int font, int maxWidth)
{
  std::string fitted = text ? text : "";
  if (fitted.empty()) {
    return fitted;
  }

  while (fitted.size() > 3 && tft.textWidth(fitted.c_str(), font) > maxWidth) {
    fitted = fitted.substr(0, fitted.size() - 4) + "...";
  }

  return fitted;
}

static void gbc_display_draw_external_info(const char* romTitle, bool colorGame)
{
  gbc_prepare_external_tft();
  tft.setTextWrap(false);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.drawRoundRect(8, 8, EXT_LCD_W - 16, EXT_LCD_H - 16, 8, TFT_DARKGREY);

  std::string title = romTitle ? romTitle : "";
  const char* drawTitle = title.empty() ? "GAME BOY" : title.c_str();
  int titleFont = 4;
  int maxTitleW = EXT_LCD_W - 32;
  if (tft.textWidth(drawTitle, 4) > maxTitleW) {
    titleFont = 2;
    if (tft.textWidth(drawTitle, 2) > maxTitleW) {
      title = truncate_text(romTitle, 36);
      title = gbc_fit_title_for_width(title.c_str(), titleFont, maxTitleW);
      drawTitle = title.c_str();
    }
  }
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawCentreString(drawTitle, EXT_LCD_W / 2, 16, titleFont);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString(colorGame ? "GAME BOY COLOR" : "GAME BOY", EXT_LCD_W / 2, 46, 2);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawCentreString(
      (g_emu_display_target == EMU_DISPLAY_INTERNAL) ? "VIDEO ON INTERNAL LCD" : "VIDEO ON EXTERNAL TFT",
      EXT_LCD_W / 2,
      64,
      2);

  tft.drawRoundRect(12, 86, EXT_LCD_W - 24, 98, 6, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("CONTROLS", EXT_LCD_W / 2, 92, 2);

  const auto actions = share::emuControlActionLabels(share::EmuProfile::Gbc);
  const auto keys    = share::emuControlKeyLabels(share::EmuProfile::Gbc);
  const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
  const size_t rowsPerCol = 4;

  for (size_t i = 0; i < count; ++i) {
    const int col = static_cast<int>(i / rowsPerCol);
    const int row = static_cast<int>(i % rowsPerCol);
    const int baseX = 24 + col * 146;
    const int baseY = 112 + row * 16;

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(actions[i].c_str(), baseX, baseY, 2);
    draw_key_badge(baseX + 88, baseY - 3, keys[i]);
  }

  tft.drawFastHLine(18, 190, EXT_LCD_W - 36, TFT_DARKGREY);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawCentreString("GO / HOLD ESC = QUIT", EXT_LCD_W / 2, 198, 1);
  tft.drawCentreString(colorGame ? "\\ = SCREEN / FN+,/ = ZOOM" : "\\ = PALETTE", EXT_LCD_W / 2, 210, 1);
}

static void gbc_display_transform(int srcW, int srcH)
{
  if (srcW <= 0 || srcH <= 0) return;

  const int targetW = gbc_target_width();
  const int targetH = gbc_target_height();
  bool full = gbcFullScreen;
  int zoom = (gbcZoomPercent > 0) ? gbcZoomPercent : 100;

  // nothing changed
  if (zoom == s_lastZoomPercent &&
      full == s_lastFullScreen &&
      srcW == s_lastSrcW &&
      srcH == s_lastSrcH &&
      targetW == s_lastTargetW &&
      targetH == s_lastTargetH) {
    return;
  }

  if (!full) {
    gbc_fill_active_screen_black();
  }

  s_lastZoomPercent = zoom;
  s_lastFullScreen  = full;
  s_lastSrcW        = srcW;
  s_lastSrcH        = srcH;
  s_lastTargetW     = targetW;
  s_lastTargetH     = targetH;

  float zoomFactor = zoom / 100.0f;
  if (zoomFactor <= 0.0f) zoomFactor = 1.0f;

  int   dstW, dstH;
  int   xOffset, yOffset;
  float baseInvScaleX, baseInvScaleY;

  if (!full) {
    float scaleX = (float)targetW / (float)srcW;
    float scaleY = (float)targetH / (float)srcH;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale <= 0.0f) scale = 1.0f;

    dstW = (int)(srcW * scale);
    dstH = (int)(srcH * scale);
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;

    xOffset = (targetW - dstW) / 2;
    yOffset = (targetH - dstH) / 2;
    if (xOffset < 0) xOffset = 0;
    if (yOffset < 0) yOffset = 0;

    baseInvScaleX = (float)srcW / (float)dstW;
    baseInvScaleY = (float)srcH / (float)dstH;
  } else {
    dstW = targetW;
    dstH = targetH;
    xOffset = 0;
    yOffset = 0;

    baseInvScaleX = (float)srcW / (float)dstW;
    baseInvScaleY = (float)srcH / (float)dstH;
  }

  float invScaleX = baseInvScaleX / zoomFactor;
  float invScaleY = baseInvScaleY / zoomFactor;

  float srcCX = (float)srcW * 0.5f;
  float srcCY = (float)srcH * 0.5f;
  float dstCX = (float)(dstW - 1) * 0.5f;
  float dstCY = (float)(dstH - 1) * 0.5f;

  s_transform.dstW    = dstW;
  s_transform.dstH    = dstH;
  s_transform.xOffset = xOffset;
  s_transform.yOffset = yOffset;

  if (dstW > s_xmapCap) {
    free(s_xmap);
    s_xmap = (int16_t*)malloc(dstW * sizeof(int16_t));
    s_xmapCap = s_xmap ? dstW : 0;
  }
  if (s_xmap) {
    for (int x = 0; x < dstW; x++) {
      int sx = (int)(srcCX + ((float)x - dstCX) * invScaleX);
      if (sx < 0) sx = 0;
      if (sx >= srcW) sx = srcW - 1;
      s_xmap[x] = (int16_t)sx;
    }
  }

  if (dstH > s_ymapCap) {
    free(s_ymap);
    s_ymap = (int16_t*)malloc(dstH * sizeof(int16_t));
    s_ymapCap = s_ymap ? dstH : 0;
  }
  if (s_ymap) {
    for (int y = 0; y < dstH; y++) {
      int sy = (int)(srcCY + ((float)y - dstCY) * invScaleY);
      if (sy < 0) sy = 0;
      if (sy >= srcH) sy = srcH - 1;
      s_ymap[y] = (int16_t)sy;
    }
  }
}

// ================== TASK ==================

static void gbc_display_task(void *arg)
{
  (void)arg;

  for (;;) {
    GbcFrameMsg msg;
    if (xQueueReceive(s_frameQ, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (!msg.fb || msg.width <= 0 || msg.height <= 0 || msg.pitch <= 0) {
      continue;
    }

    gbc_display_transform(msg.width, msg.height);

    const int dstW = s_transform.dstW;
    const int dstH = s_transform.dstH;
    const int xOff = s_transform.xOffset;
    const int yOff = s_transform.yOffset;

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

    // Bottom cache: snapshot ~60% of bottom framebuffer (tearing reduction)
    const int srcW = msg.width;
    const int srcH = msg.height;
    {
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
                 msg.fb + ySrc * msg.pitch,
                 srcW * sizeof(uint16_t));
        }
      }
    }

    const bool gameOnInternal = gbc_game_on_internal();

    const bool use12 = !gameOnInternal && (g_emu_color_depth == EMU_COLOR_12BIT);

    if (gameOnInternal) {
      M5Cardputer.Display.startWrite();
      M5Cardputer.Display.setAddrWindow(xOff, yOff, dstW, dstH);
    } else {
      tft.startWrite();
      // Re-assert COLMOD every frame (safety net)
      if (use12) {
        tft.writecommand(0x3A);
        tft.writedata(0x53);
      }
      tft.setAddrWindow(xOff, yOff, dstW, dstH);
    }

    for (int y = 0; y < dstH; ++y) {
      int srcY = s_ymap[y];
      const uint16_t *srcLine = nullptr;

      // Use bottom cache if srcY is in the cached range
      if (s_bottomCache && srcY >= s_bottomCacheStartY) {
        int idx = srcY - s_bottomCacheStartY;
        if (idx >= 0 && idx < s_bottomCacheLines) {
          srcLine = s_bottomCache + idx * s_bottomCacheW;
        }
      }
      if (!srcLine) {
        srcLine = msg.fb + srcY * msg.pitch;
      }

      if (use12) {
        // Identical to PCE 12-bit: read LE source → pack to RGB444
        // NO in-place aliasing (reads from srcLine, writes to buf12)
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
        tft.pushColors((uint16_t*)buf12, (byteCount + 1) / 2, false);
      } else {
        // 16-bit: fill line buffer then push
        for (int x = 0; x < dstW; ++x) {
          s_lineBuf[x] = srcLine[s_xmap[x]];
        }
        if (gameOnInternal) {
          M5Cardputer.Display.writePixels(s_lineBuf, dstW, true);
        } else {
          tft.pushColors(s_lineBuf, dstW);
        }
      }
    }

    if (gameOnInternal) {
      M5Cardputer.Display.endWrite();
    } else {
      tft.endWrite();
    }

    vTaskDelay(0);
  }
}

// ================== PUBLIC API ==================

extern "C" void gbc_display_set_target(gbc_display_target_t target)
{
  s_target = target;
  gbc_reset_transform_cache();

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setSwapBytes(true);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  if (target == GBC_DISPLAY_EXTERNAL) {
    tft.begin();
    tft.setRotation(3);
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);
    if (g_emu_color_depth == EMU_COLOR_12BIT) {
      // Keep CS LOW throughout command+parameter to ensure COLMOD sticks
      tft.startWrite();
      tft.writecommand(0x3A);  // COLMOD
      tft.writedata(0x53);     // DPI=16bit, DBI=12bit (RGB444)
      tft.endWrite();
    }
  }
}

extern "C" void gbc_display_init(void)
{
  s_target = (g_emu_display_target == EMU_DISPLAY_INTERNAL)
      ? GBC_DISPLAY_INTERNAL
      : GBC_DISPLAY_EXTERNAL;

  if (!gbc_game_on_internal()) {
    tft.begin();
    tft.setRotation(3); // Landscape
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);
  }

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setSwapBytes(true);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  gbc_reset_transform_cache();

  if (!s_frameQ) {
    s_frameQ = xQueueCreate(2, sizeof(GbcFrameMsg));  // 2 frames max
    if (!s_frameQ) {
      printf("[GBC-DISP] queue create failed\n");
    }
  }
}

extern "C" void gbc_display_start(void)
{
  if (!s_frameQ) {
    gbc_display_init();
  }
  if (!s_task && s_frameQ) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      gbc_display_task,
      "GbcDisp",
      3072,
      nullptr,
      3,
      &s_task,
      1
    );
    if (ok != pdPASS) {
      printf("[GBC-DISP] task create failed\n");
      if (s_task) {
        vTaskDelete(s_task);
      }
      s_task = nullptr;
    }
  }
}

extern "C" void gbc_display_show_external_info(const char* romTitle, bool colorGame)
{
  gbc_display_draw_external_info(romTitle, colorGame);
}

extern "C" void gbc_display_stop(void)
{
  if (s_task) {
    vTaskDelete(s_task);
    s_task = nullptr;
  }
  if (s_frameQ) {
    vQueueDelete(s_frameQ);
    s_frameQ = nullptr;
  }
  free(s_lineBuf);
  s_lineBuf = nullptr;
  s_lineCap = 0;
  free(s_bottomCache);
  s_bottomCache = nullptr;
  s_bottomCacheLines = 0;
  s_bottomCacheW = 0;
  free(s_xmap);
  s_xmap = nullptr;
  s_xmapCap = 0;
  free(s_ymap);
  s_ymap = nullptr;
  s_ymapCap = 0;

  gbc_reset_transform_cache();
}

extern "C" void gbc_display_submit_frame(const uint16_t *fb,
                                         int pitch,
                                         int width,
                                         int height)
{
  if (!s_frameQ || !fb) return;

  GbcFrameMsg msg;
  msg.fb     = fb;
  msg.pitch  = pitch;
  msg.width  = width;
  msg.height = height;

  BaseType_t ok = xQueueSend(s_frameQ, &msg, 0);
  (void)ok;
}
