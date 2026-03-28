#include "pce_display.h"
#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include "../tft_setup.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "esp_heap_caps.h"
#include <string>

extern "C" {
  #include "pce-go/pce.h"
  #include "pce-go/psg.h"
  void* PalettePCE(int brightness);
}

typedef struct {
  const uint8_t  *index_fb_base;
  const uint16_t *palette;
  int pitch;
  int width;
  int height;
} PceFrameMsg;

// External TFT instance (file-scoped)
static TFT_eSPI s_tft;

// External TFT resolution
static constexpr int EXT_W = 320;
static constexpr int EXT_H = 240;

// Globals
static QueueHandle_t  s_frameQ   = nullptr;
static TaskHandle_t   s_task     = nullptr;
static uint16_t      *s_lineBuf  = nullptr;
static int            s_lineCap  = 0;

static uint8_t*  pce_index_fb_base = nullptr;
static uint8_t*  pce_index_fb      = nullptr; // start visible (offset 16)
static uint16_t *pce_palette       = nullptr;
static int       pce_fb_width  = 0;
static int       pce_fb_height = 0;
static int       pce_fb_pitch  = 0;

bool pceFullScreen = true;
int  pceZoomLevel = 110;

// Integer LUTs for scaling
static int16_t *s_xmap    = nullptr;
static int16_t *s_ymap    = nullptr;
static int      s_xmapCap = 0;
static int      s_ymapCap = 0;

// Cached transform
static int  s_dstW, s_dstH, s_xOff, s_yOff;
static int  s_lastZoom     = -1;
static bool s_lastFull     = false;
static int  s_lastSrcW     = 0;
static int  s_lastSrcH     = 0;
static int  s_lastTargetW  = 0;
static int  s_lastTargetH  = 0;

// Bottom cache (tearing reduction)
static uint8_t* s_bottomCache        = nullptr;
static int      s_bottomCacheLines   = 0;
static int      s_bottomCacheW       = 0;
static int      s_bottomCacheStartY  = 0;

static inline bool pce_on_external()
{
  return g_emu_display_target == EMU_DISPLAY_EXTERNAL;
}

static int pce_target_w()
{
  return pce_on_external() ? EXT_W : M5Cardputer.Display.width();
}

static int pce_target_h()
{
  return pce_on_external() ? EXT_H : M5Cardputer.Display.height();
}

static inline int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static void pce_build_luts(int srcW, int srcH)
{
  bool full = pceFullScreen;
  int  zoom = (pceZoomLevel > 0) ? pceZoomLevel : 100;
  int  tgtW = pce_target_w();
  int  tgtH = pce_target_h();

  if (zoom == s_lastZoom && full == s_lastFull &&
      srcW == s_lastSrcW && srcH == s_lastSrcH &&
      tgtW == s_lastTargetW && tgtH == s_lastTargetH) {
    return;
  }

  s_lastZoom    = zoom;
  s_lastFull    = full;
  s_lastSrcW    = srcW;
  s_lastSrcH    = srcH;
  s_lastTargetW = tgtW;
  s_lastTargetH = tgtH;

  int roiX0 = 0, roiY0 = 0, roiW = srcW, roiH = srcH;

  if (full && zoom > 100) {
    roiW = clampi((int)((uint32_t)srcW * 100u / (uint32_t)zoom), 16, srcW);
    roiH = clampi((int)((uint32_t)srcH * 100u / (uint32_t)zoom), 16, srcH);
    roiX0 = (srcW - roiW) / 2;
    roiY0 = (srcH - roiH) / 2;
  }

  int dstW, dstH, xOff, yOff;

  if (!full) {
    // Aspect-ratio preserving
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
    if (pce_on_external()) {
      s_tft.fillScreen(TFT_BLACK);
    } else {
      M5Cardputer.Display.fillScreen(TFT_BLACK);
    }
  } else {
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

// ================== DISPLAY TASK ==================

static void pce_display_task(void *arg) {
  for (;;) {
    PceFrameMsg msg;
    if (xQueueReceive(s_frameQ, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (!msg.index_fb_base || !msg.palette || msg.width <= 0 || msg.height <= 0) {
      continue;
    }

    int srcW  = msg.width;
    int srcH  = msg.height;
    int pitch = msg.pitch;

    pce_build_luts(srcW, srcH);
    if (!s_xmap || !s_ymap) continue;

    int dstW = s_dstW;
    int dstH = s_dstH;
    int xOff = s_xOff;
    int yOff = s_yOff;

    if (dstW <= 0 || dstH <= 0) continue;

    // Allocate RGB line buffer
    int maxW = pce_on_external() ? EXT_W : M5Cardputer.Display.width();
    if (dstW > maxW) maxW = dstW;
    if (maxW > s_lineCap) {
      free(s_lineBuf);
      s_lineBuf = (uint16_t*)heap_caps_malloc(maxW * sizeof(uint16_t),
                                              MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
      if (!s_lineBuf) {
        s_lineBuf = (uint16_t*)malloc(maxW * sizeof(uint16_t));
      }
      s_lineCap = s_lineBuf ? maxW : 0;
    }
    if (!s_lineBuf) continue;

    // Bottom cache: snapshot ~55% of bottom framebuffer (tearing reduction)
    int cacheLines = (srcH * 60 + 99) / 100;
    if (cacheLines < 1) cacheLines = 1;
    int cacheStartY = srcH - cacheLines;
    if (cacheStartY < 0) cacheStartY = 0;
    int neededCacheBytes = cacheLines * srcW;

    if (!s_bottomCache || neededCacheBytes > (s_bottomCacheLines * s_bottomCacheW)) {
      free(s_bottomCache);
      s_bottomCache = (uint8_t*)heap_caps_malloc(neededCacheBytes, MALLOC_CAP_8BIT);
      if (!s_bottomCache) {
        printf("[PCE] bottomCache alloc failed (%d bytes)\n", neededCacheBytes);
      } else {
        s_bottomCacheLines = cacheLines;
        s_bottomCacheW     = srcW;
      }
    }
    s_bottomCacheStartY = cacheStartY;

    // Snapshot bottom into cache
    if (s_bottomCache) {
      for (int i = 0; i < cacheLines; ++i) {
        int ySrc = cacheStartY + i;
        if (ySrc >= srcH) break;
        const uint8_t* srcLine = msg.index_fb_base + ySrc * pitch + 16;
        memcpy(s_bottomCache + i * srcW, srcLine, srcW);
      }
    }

    const bool ext = pce_on_external();

    // Single setAddrWindow for the entire frame
    if (ext) {
      s_tft.startWrite();
      s_tft.setAddrWindow(xOff, yOff, dstW, dstH);
    } else {
      M5Cardputer.Display.startWrite();
      M5Cardputer.Display.setAddrWindow(xOff, yOff, dstW, dstH);
    }

    const bool use12 = ext && (g_emu_color_depth == EMU_COLOR_12BIT);

    for (int y = 0; y < dstH; ++y) {
      int srcY = s_ymap[y];

      const uint8_t* srcLine = nullptr;

      // Use bottom cache if srcY is in the cached range
      if (s_bottomCache && srcY >= s_bottomCacheStartY) {
        int idxLine = srcY - s_bottomCacheStartY;
        if (idxLine >= 0 && idxLine < s_bottomCacheLines) {
          srcLine = s_bottomCache + idxLine * srcW;
        }
      }

      // Direct read from framebuffer
      if (!srcLine) {
        srcLine = msg.index_fb_base + srcY * pitch + 16;
      }

      if (use12) {
        // RGB444 packed: 2 pixels → 3 bytes (PCE palette is native LE RGB565)
        uint8_t *buf12 = (uint8_t*)s_lineBuf;
        int pairs = dstW / 2;
        for (int p = 0; p < pairs; p++) {
          uint16_t c1 = msg.palette[srcLine[s_xmap[p * 2]]];
          uint16_t c2 = msg.palette[srcLine[s_xmap[p * 2 + 1]]];
          int j = p * 3;
          buf12[j]   = ((c1 >> 8) & 0xF0) | ((c1 >> 7) & 0x0F);
          buf12[j+1] = ((c1 << 3) & 0xF0) | ((c2 >> 12) & 0x0F);
          buf12[j+2] = ((c2 >> 3) & 0xF0) | ((c2 >> 1) & 0x0F);
        }
        if (dstW & 1) {
          uint16_t c = msg.palette[srcLine[s_xmap[dstW - 1]]];
          int j = pairs * 3;
          buf12[j]   = ((c >> 8) & 0xF0) | ((c >> 7) & 0x0F);
          buf12[j+1] = ((c << 3) & 0xF0);
          buf12[j+2] = 0;
        }
        int byteCount = ((dstW + 1) / 2) * 3;
        s_tft.pushColors((uint16_t*)buf12, (byteCount + 1) / 2, false);
      } else {
        for (int x = 0; x < dstW; ++x) {
          uint8_t idx = srcLine[s_xmap[x]];
          s_lineBuf[x] = msg.palette[idx];
        }
        if (ext) {
          s_tft.pushColors(s_lineBuf, dstW, true);
        } else {
          M5Cardputer.Display.pushPixels(s_lineBuf, dstW);
        }
      }
    }

    if (ext) {
      s_tft.endWrite();
    } else {
      M5Cardputer.Display.endWrite();
    }
  }
}

// ================== EXTERNAL INFO SCREEN ==================

static std::string pce_truncate(const char* text, size_t maxChars)
{
  if (!text) return "";
  std::string v(text);
  if (v.size() <= maxChars) return v;
  if (maxChars <= 3) return v.substr(0, maxChars);
  return v.substr(0, maxChars - 3) + "...";
}

static void pce_draw_key_badge(int x, int y, const std::string& key)
{
  const int bw = 34, bh = 18;
  s_tft.fillRoundRect(x, y, bw, bh, 4, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  s_tft.drawCentreString(key.c_str(), x + bw / 2, y + 1, 2);
  s_tft.drawRoundRect(x, y, bw, bh, 4, TFT_YELLOW);
}

extern "C" void pce_display_show_external_info(const char* romTitle)
{
  s_tft.begin();
  s_tft.setRotation(3);
  s_tft.fillScreen(TFT_BLACK);
  s_tft.setTextWrap(false);

  s_tft.drawRoundRect(8, 8, EXT_W - 16, EXT_H - 16, 8, TFT_DARKGREY);

  std::string title = romTitle ? romTitle : "";
  const char* drawTitle = title.empty() ? "PC ENGINE" : title.c_str();
  int titleFont = 4;
  int maxTitleW = EXT_W - 32;
  if (s_tft.textWidth(drawTitle, 4) > maxTitleW) {
    titleFont = 2;
    if (s_tft.textWidth(drawTitle, 2) > maxTitleW) {
      title = pce_truncate(romTitle, 36);
      drawTitle = title.c_str();
    }
  }
  s_tft.setTextColor(TFT_CYAN, TFT_BLACK);
  s_tft.drawCentreString(drawTitle, EXT_W / 2, 16, titleFont);

  s_tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  s_tft.drawCentreString("PC ENGINE / TurboGrafx-16", EXT_W / 2, 46, 2);

  s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
  s_tft.drawCentreString("VIDEO ON INTERNAL LCD", EXT_W / 2, 64, 2);

  s_tft.drawRoundRect(12, 86, EXT_W - 24, 98, 6, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft.drawCentreString("CONTROLS", EXT_W / 2, 92, 2);

  const auto actions = share::emuControlActionLabels(share::EmuProfile::Pce);
  const auto keys    = share::emuControlKeyLabels(share::EmuProfile::Pce);
  const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
  const size_t rowsPerCol = 4;

  for (size_t i = 0; i < count; ++i) {
    const int col = (int)(i / rowsPerCol);
    const int row = (int)(i % rowsPerCol);
    const int baseX = 24 + col * 146;
    const int baseY = 112 + row * 16;

    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawString(actions[i].c_str(), baseX, baseY, 2);
    pce_draw_key_badge(baseX + 88, baseY - 3, keys[i]);
  }

  s_tft.drawFastHLine(18, 190, EXT_W - 36, TFT_DARKGREY);
  s_tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  s_tft.drawCentreString("GO / HOLD ESC = QUIT", EXT_W / 2, 198, 1);
  s_tft.drawCentreString("\\ = SCREEN  FN+,/ = ZOOM", EXT_W / 2, 210, 1);
}

// ================== API DISPLAY ==================

extern "C" void pce_display_init(void) {
  // Initialize external TFT if needed
  if (pce_on_external()) {
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

  uint16_t* corePal = (uint16_t*)PalettePCE(16);
  if (!corePal) {
    printf("[PCE][ERR] PalettePCE(16) returned NULL\n");
    for(;;) delay(1000);
  }

  if (!pce_palette) {
    pce_palette = (uint16_t*)heap_caps_malloc(
      256 * sizeof(uint16_t),
      MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!pce_palette) {
      printf("[PCE][ERR] pce_palette alloc failed\n");
      free(corePal);
      for(;;) delay(1000);
    }
  }

  for (int i = 0; i < 256; i++) {
    pce_palette[i] = corePal[i];
  }

  free(corePal);

  if (!s_frameQ) {
    s_frameQ = xQueueCreate(2, sizeof(PceFrameMsg));
    if (!s_frameQ) {
      printf("[PCE-DISP] queue create failed\n");
    }
  }

  s_lastZoom = -1;  // force LUT rebuild
}

extern "C" void pce_display_start(void) {
  if (!s_frameQ) pce_display_init();
  if (!s_task && s_frameQ) {
    // Core 1, priority 3 (below Speaker hw task at 4)
    BaseType_t ok = xTaskCreatePinnedToCore(
      pce_display_task, "PceDisp",
      4096, nullptr, 3, &s_task,
      1
    );
    if (ok != pdPASS) {
      printf("[PCE-DISP] task create failed\n");
      if (s_task) {
        vTaskDelete(s_task);
      }
      s_task = nullptr;
    }
  }
}

extern "C" void pce_display_stop(void) {
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
  if (pce_palette) {
    heap_caps_free(pce_palette);
    pce_palette = nullptr;
  }
  free(s_xmap);  s_xmap = nullptr; s_xmapCap = 0;
  free(s_ymap);  s_ymap = nullptr; s_ymapCap = 0;
  free(s_bottomCache); s_bottomCache = nullptr;
  s_bottomCacheLines = 0;
  s_bottomCacheW = 0;
}

extern "C" void pce_display_submit_frame(const uint8_t *index_fb_base,
                                         int pitch,
                                         int width,
                                         int height,
                                         const uint16_t *palette)
{
  if (!s_frameQ) return;

  PceFrameMsg msg;
  msg.index_fb_base = index_fb_base;
  msg.palette       = palette;
  msg.pitch         = pitch;
  msg.width         = width;
  msg.height        = height;

  // drop if full
  xQueueSend(s_frameQ, &msg, 0);
}

// ================== HOOKS PCE-GO ==================

extern "C" uint8_t* osd_gfx_framebuffer(int width, int height)
{
  if (width <= 0 || height <= 0) {
    return pce_index_fb;
  }

  pce_fb_width  = width;
  pce_fb_height = height;
  pce_fb_pitch  = XBUF_WIDTH;

  if (!pce_index_fb_base) {
    size_t indexBytes = (size_t)pce_fb_pitch * (size_t)pce_fb_height;

    pce_index_fb_base = (uint8_t*)heap_caps_malloc(
        indexBytes,
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!pce_index_fb_base) {
      printf("[PCE] index_fb_base alloc failed\n");
      return nullptr;
    }

    // overscan of 16 pixels
    pce_index_fb = pce_index_fb_base + 16;
  }

  return pce_index_fb;
}

extern "C" void osd_vsync(void)
{
  // send the frame to the display thread (non-blocking)
  pce_display_submit_frame(pce_index_fb_base,
                           pce_fb_pitch,
                           pce_fb_width,
                           pce_fb_height,
                           pce_palette);
}
