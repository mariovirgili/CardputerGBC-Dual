// nes_display.cpp — Dual-target display (Internal M5.Lcd / External TFT_eSPI)

#include <Arduino.h>
#include <M5Unified.h>
#include <TFT_eSPI.h>
#include "../tft_setup.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "esp_heap_caps.h"
#include <string>

extern "C" {
#include <nes/nes.h>
}

// External TFT instance (file-scoped)
static TFT_eSPI s_tft;

// External TFT resolution
static constexpr int EXT_W = 320;
static constexpr int EXT_H = 240;

extern uint16_t *myPalette;
extern bool fullscreenMode;
extern int nesZoomPercent;

static uint16_t *s_nesLineBuf = nullptr;
static int       s_nesLineBufLen = 0;

// Native LE copy of the NES palette (for 12-bit path — avoids per-pixel byte-swap)
static uint16_t  s_nesLePal[256];

// Full-frame cache (tearing elimination — snapshot entire framebuffer)
static uint8_t* s_frameCache     = nullptr;
static int      s_frameCacheW    = 0;
static int      s_frameCacheH    = 0;

// Integer LUTs for scaling
static int16_t *s_xmap    = nullptr;
static int16_t *s_ymap    = nullptr;
static int      s_xmapCap = 0;
static int      s_ymapCap = 0;

// Cached transform
static int  s_dstW, s_dstH, s_xOff, s_yOff;
static int  s_lastZoom    = -1;
static bool s_lastFull    = false;
static int  s_lastTargetW = 0;
static int  s_lastTargetH = 0;

static inline bool nes_on_external()
{
  return g_emu_display_target == EMU_DISPLAY_EXTERNAL;
}

static int nes_target_w()
{
  return nes_on_external() ? EXT_W : M5.Lcd.width();
}

static int nes_target_h()
{
  return nes_on_external() ? EXT_H : M5.Lcd.height();
}

static inline int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static void nes_build_luts()
{
  bool full = fullscreenMode;
  int  zoom = (nesZoomPercent > 0) ? nesZoomPercent : 100;
  int  tgtW = nes_target_w();
  int  tgtH = nes_target_h();

  if (zoom == s_lastZoom && full == s_lastFull &&
      tgtW == s_lastTargetW && tgtH == s_lastTargetH) {
    return;
  }

  s_lastZoom    = zoom;
  s_lastFull    = full;
  s_lastTargetW = tgtW;
  s_lastTargetH = tgtH;

  const int srcW = NES_SCREEN_WIDTH;   // 256
  const int srcH = NES_SCREEN_HEIGHT;  // 240

  int roiX0 = 0, roiY0 = 0, roiW = srcW, roiH = srcH;

  if (full && zoom > 100) {
    roiW = clampi((int)((uint32_t)srcW * 100u / (uint32_t)zoom), 16, srcW);
    roiH = clampi((int)((uint32_t)srcH * 100u / (uint32_t)zoom), 16, srcH);
    roiX0 = (srcW - roiW) / 2;
    roiY0 = (srcH - roiH) / 2;
  }

  int dstW, dstH, xOff, yOff;

  if (!full) {
    // Aspect-ratio preserving: fit NES 256x240 into target
    float scaleX = (float)tgtW / (float)srcW;
    float scaleY = (float)tgtH / (float)srcH;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale <= 0.0f) scale = 1.0f;

    dstW = (int)(srcW * scale);
    dstH = (int)(srcH * scale);
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;

    xOff = (tgtW - dstW) / 2;
    yOff = (tgtH - dstH) / 2;
    if (xOff < 0) xOff = 0;
    if (yOff < 0) yOff = 0;

    // Clear borders
    if (nes_on_external()) {
      s_tft.fillScreen(TFT_BLACK);
    } else {
      M5.Lcd.fillScreen(TFT_BLACK);
    }
  } else {
    // Fullscreen: stretch to fill target
    dstW = tgtW;
    dstH = tgtH;
    xOff = 0;
    yOff = 0;
  }

  s_dstW = dstW;
  s_dstH = dstH;
  s_xOff = xOff;
  s_yOff = yOff;

  // Build X LUT
  if (dstW > s_xmapCap) {
    free(s_xmap);
    s_xmap = (int16_t*)malloc(dstW * sizeof(int16_t));
    s_xmapCap = s_xmap ? dstW : 0;
  }
  if (s_xmap) {
    for (int x = 0; x < dstW; x++) {
      int sx = roiX0 + (x * roiW) / dstW;
      s_xmap[x] = (int16_t)clampi(sx, 0, srcW - 1);
    }
  }

  // Build Y LUT
  if (dstH > s_ymapCap) {
    free(s_ymap);
    s_ymap = (int16_t*)malloc(dstH * sizeof(int16_t));
    s_ymapCap = s_ymap ? dstH : 0;
  }
  if (s_ymap) {
    for (int y = 0; y < dstH; y++) {
      int sy = roiY0 + (y * roiH) / dstH;
      s_ymap[y] = (int16_t)clampi(sy, 0, srcH - 1);
    }
  }
}

// ================== PUBLIC API (called from nes_osd.c) ==================

extern "C" {

void display_begin() {}

void display_init()
{
  if (nes_on_external()) {
    s_tft.begin();
    s_tft.setRotation(3);
    s_tft.fillScreen(TFT_BLACK);
    if (g_emu_color_depth == EMU_COLOR_12BIT) {
      s_tft.startWrite();
      s_tft.writecommand(0x3A);  // COLMOD
      s_tft.writedata(0x53);     // DPI=16bit, DBI=12bit (RGB444)
      s_tft.endWrite();
    }
  }

  // Build native LE palette for 12-bit path (avoids per-pixel byte-swap)
  if (myPalette) {
    for (int i = 0; i < 256; i++) {
      uint16_t cbe = myPalette[i];
      s_nesLePal[i] = (cbe >> 8) | (cbe << 8);
    }
  }

  int maxW = nes_on_external() ? EXT_W : M5.Lcd.width();

  if (!s_nesLineBuf || s_nesLineBufLen < maxW) {
    free(s_nesLineBuf);
    s_nesLineBuf = (uint16_t*)heap_caps_malloc(
        maxW * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_nesLineBuf) {
      s_nesLineBuf = (uint16_t*)malloc(maxW * sizeof(uint16_t));
    }
    s_nesLineBufLen = s_nesLineBuf ? maxW : 0;
  }
  if (s_nesLineBuf) {
    memset(s_nesLineBuf, 0, maxW * sizeof(uint16_t));
  }

  s_lastZoom = -1;  // force LUT rebuild
}

void display_write_frame(const uint8_t *data[])
{
  if (!s_nesLineBuf || !myPalette || !data) return;

  nes_build_luts();
  if (!s_xmap || !s_ymap) return;

  int dstW = s_dstW;
  int dstH = s_dstH;
  int xOff = s_xOff;
  int yOff = s_yOff;

  const int srcW = NES_SCREEN_WIDTH;   // 256
  const int srcH = NES_SCREEN_HEIGHT;  // 240

  // Full-frame snapshot: copy entire framebuffer to eliminate tearing
  // (emulator core writes fb on core 0 while display task reads on core 1)
  {
    int neededBytes = srcW * srcH;  // 256×240 = 61440 bytes

    if (!s_frameCache || s_frameCacheW != srcW || s_frameCacheH != srcH) {
      free(s_frameCache);
      // Try PSRAM first (plenty of room), fall back to internal RAM
      s_frameCache = (uint8_t*)heap_caps_malloc(neededBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!s_frameCache)
        s_frameCache = (uint8_t*)heap_caps_malloc(neededBytes, MALLOC_CAP_8BIT);
      s_frameCacheW = s_frameCache ? srcW : 0;
      s_frameCacheH = s_frameCache ? srcH : 0;
    }

    if (s_frameCache) {
      for (int i = 0; i < srcH; ++i) {
        if (!data[i]) break;
        memcpy(s_frameCache + i * srcW, data[i], srcW);
      }
    }
  }

  const bool ext = nes_on_external();
  const bool use12 = ext && (g_emu_color_depth == EMU_COLOR_12BIT);

  // Refresh LE palette every frame (NES palette can change at runtime)
  if (use12 && myPalette) {
    for (int i = 0; i < 256; i++) {
      uint16_t cbe = myPalette[i];
      s_nesLePal[i] = (cbe >> 8) | (cbe << 8);
    }
  }

  if (ext) {
    s_tft.startWrite();
    // Re-assert COLMOD every frame (safety net — s_tft.begin() elsewhere resets to 16-bit)
    if (use12) {
      s_tft.writecommand(0x3A);
      s_tft.writedata(0x53);
    }
    s_tft.setAddrWindow(xOff, yOff, dstW, dstH);
  } else {
    M5.Lcd.startWrite();
    M5.Lcd.setAddrWindow(xOff, yOff, dstW, dstH);
  }

  for (int y = 0; y < dstH; y++) {
    int srcY = s_ymap[y];
    const uint8_t *srcLine;

    if (s_frameCache && srcY < s_frameCacheH) {
      srcLine = s_frameCache + srcY * s_frameCacheW;
    } else {
      srcLine = data[srcY];
    }

    if (use12) {
      // Identical to PCE 12-bit path: LE palette lookup → pack to RGB444
      // NO in-place aliasing (reads from palette+fb, writes to buf12)
      uint8_t *buf12 = (uint8_t*)s_nesLineBuf;
      int pairs = dstW / 2;
      for (int p = 0; p < pairs; p++) {
        uint16_t c1 = s_nesLePal[srcLine[s_xmap[p * 2]]];
        uint16_t c2 = s_nesLePal[srcLine[s_xmap[p * 2 + 1]]];
        int j = p * 3;
        buf12[j]   = ((c1 >> 8) & 0xF0) | ((c1 >> 7) & 0x0F);
        buf12[j+1] = ((c1 << 3) & 0xF0) | ((c2 >> 12) & 0x0F);
        buf12[j+2] = ((c2 >> 3) & 0xF0) | ((c2 >> 1) & 0x0F);
      }
      if (dstW & 1) {
        uint16_t c = s_nesLePal[srcLine[s_xmap[dstW - 1]]];
        int j = pairs * 3;
        buf12[j]   = ((c >> 8) & 0xF0) | ((c >> 7) & 0x0F);
        buf12[j+1] = ((c << 3) & 0xF0);
        buf12[j+2] = 0;
      }
      int byteCount = ((dstW + 1) / 2) * 3;
      s_tft.pushColors((uint16_t*)buf12, (byteCount + 1) / 2, false);
    } else {
      for (int x = 0; x < dstW; x++) {
        s_nesLineBuf[x] = myPalette[srcLine[s_xmap[x]]];
      }
      if (ext) {
        s_tft.pushColors(s_nesLineBuf, dstW, false);
      } else {
        M5.Lcd.pushPixels(s_nesLineBuf, dstW);
      }
    }
  }

  if (ext) {
    s_tft.endWrite();
  } else {
    M5.Lcd.endWrite();
  }
}

void display_clear()
{
  if (nes_on_external()) {
    s_tft.fillScreen(TFT_BLACK);
  } else {
    M5.Lcd.fillScreen(TFT_BLACK);
  }
}

} // extern "C"

// ================== EXTERNAL INFO SCREEN ==================

static std::string nes_truncate(const char* text, size_t maxChars)
{
  if (!text) return "";
  std::string v(text);
  if (v.size() <= maxChars) return v;
  if (maxChars <= 3) return v.substr(0, maxChars);
  return v.substr(0, maxChars - 3) + "...";
}

static void nes_draw_key_badge(int x, int y, const std::string& key)
{
  const int bw = 34, bh = 18;
  s_tft.fillRoundRect(x, y, bw, bh, 4, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  s_tft.drawCentreString(key.c_str(), x + bw / 2, y + 4, 2);
  s_tft.drawRoundRect(x, y, bw, bh, 4, TFT_YELLOW);
}

void nes_display_show_external_info(const char* romTitle)
{
  s_tft.begin();
  s_tft.setRotation(3);
  s_tft.fillScreen(TFT_BLACK);
  s_tft.setTextWrap(false);

  s_tft.drawRoundRect(8, 8, EXT_W - 16, EXT_H - 16, 8, TFT_DARKGREY);

  std::string title = romTitle ? romTitle : "";
  const char* drawTitle = title.empty() ? "NES" : title.c_str();
  int titleFont = 4;
  int maxTitleW = EXT_W - 32;
  if (s_tft.textWidth(drawTitle, 4) > maxTitleW) {
    titleFont = 2;
    if (s_tft.textWidth(drawTitle, 2) > maxTitleW) {
      title = nes_truncate(romTitle, 36);
      drawTitle = title.c_str();
    }
  }
  s_tft.setTextColor(TFT_CYAN, TFT_BLACK);
  s_tft.drawCentreString(drawTitle, EXT_W / 2, 16, titleFont);

  s_tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  s_tft.drawCentreString("NINTENDO ENTERTAINMENT SYSTEM", EXT_W / 2, 46, 2);

  s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
  s_tft.drawCentreString("VIDEO ON INTERNAL LCD", EXT_W / 2, 64, 2);

  s_tft.drawRoundRect(12, 86, EXT_W - 24, 98, 6, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft.drawCentreString("CONTROLS", EXT_W / 2, 92, 2);

  const auto actions = share::emuControlActionLabels(share::EmuProfile::Nes);
  const auto keys    = share::emuControlKeyLabels(share::EmuProfile::Nes);
  const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
  const size_t rowsPerCol = 4;

  for (size_t i = 0; i < count; ++i) {
    const int col = (int)(i / rowsPerCol);
    const int row = (int)(i % rowsPerCol);
    const int baseX = 24 + col * 146;
    const int baseY = 112 + row * 16;

    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawString(actions[i].c_str(), baseX, baseY, 2);
    nes_draw_key_badge(baseX + 88, baseY - 3, keys[i]);
  }

  s_tft.drawFastHLine(18, 190, EXT_W - 36, TFT_DARKGREY);
  s_tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  s_tft.drawString("GO = QUIT", 18, 198, 1);
  s_tft.drawString("HOLD GO = CONFIG", 100, 198, 1);
  s_tft.drawString("\\ = SCREEN  FN+,/ = ZOOM", 18, 210, 1);
}
