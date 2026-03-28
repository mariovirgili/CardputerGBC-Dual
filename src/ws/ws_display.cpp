extern "C" {
  #include "oswan/WS.h"
  #include "oswan/WSRender.h"
}

#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include "../tft_setup.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <esp_attr.h>
#include <string>

// External TFT instance (file-scoped)
static TFT_eSPI s_tft;
static constexpr int EXT_W = 320;
static constexpr int EXT_H = 240;
static bool s_use_ext   = false;
static bool s_use_12bit = false;

// Dimensions WonderSwan (src)
static const int kSrcW = LCD_MAIN_W;   // 224
static const int kSrcH = LCD_MAIN_H;   // 144

static const int INT_W = 240;
static const int INT_H = 135;

// Threading
static TaskHandle_t s_wsDispTask = nullptr;

// State & buffers
bool      ws_fullscreen     = true;
int       ws_zoomPercent    = 100;
static int lastZoomPercent = -1;
static bool lastFullscreen = true;

static uint16_t* s_line16 = nullptr;
static uint16_t* s_xmap   = nullptr;
static uint16_t* s_ymap   = nullptr;
static int s_dstW = 0, s_dstH = 0;
static int s_offX = 0, s_offY = 0;
static int s_lineCap = 0;

// Bottom cache (tearing reduction)
static uint16_t* s_bottomCache       = nullptr;
static int       s_bottomCacheLines  = 0;
static int       s_bottomCacheW      = 0;
static int       s_bottomCacheStartY = 0;

static inline int ws_target_w() {
  return s_use_ext ? EXT_W : INT_W;
}
static inline int ws_target_h() {
  return s_use_ext ? EXT_H : INT_H;
}
static inline int ws_fb_stride() {
  return SCREEN_WIDTH;
}

static void ws_display_free_buffers() {
  free(s_line16); s_line16 = nullptr; s_lineCap = 0;
  free(s_xmap);   s_xmap   = nullptr;
  free(s_ymap);   s_ymap   = nullptr;
  free(s_bottomCache); s_bottomCache = nullptr;
  s_bottomCacheLines = 0;
  s_bottomCacheW = 0;
}

static void ws_display_compute_scaler() {
  const int tgtW = ws_target_w();
  const int tgtH = ws_target_h();

  if (s_use_ext) {
    // Don't touch internal display state
  } else {
    M5Cardputer.Display.setSwapBytes(true);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
  }

  if (ws_fullscreen) {
    s_dstW = tgtW; s_dstH = tgtH;
    s_offX = 0;    s_offY = 0;
  } else {
    const float sx = (float)tgtW / (float)kSrcW;
    const float sy = (float)tgtH / (float)kSrcH;
    const float s  = (sx < sy) ? sx : sy;
    s_dstW = std::max(1, (int)(kSrcW * s));
    s_dstH = std::max(1, (int)(kSrcH * s));
    s_offX = (tgtW - s_dstW) / 2;
    s_offY = (tgtH - s_dstH) / 2;
    if (s_use_ext) {
      s_tft.fillScreen(TFT_BLACK);
    }
  }

  // ROI zoom
  int zp   = (ws_zoomPercent <= 0) ? 100 : ws_zoomPercent;
  int roiW = kSrcW * 100 / zp;
  int roiH = kSrcH * 100 / zp;
  roiW = std::min(kSrcW, std::max(16, roiW));
  roiH = std::min(kSrcH, std::max(16, roiH));
  const int roiX0 = (kSrcW - roiW) / 2;
  const int roiY0 = (kSrcH - roiH) / 2;

  // Allocate LUTs
  int maxW = std::max(tgtW, s_dstW);
  if (!s_xmap) s_xmap = (uint16_t*)heap_caps_malloc(maxW * sizeof(uint16_t), MALLOC_CAP_8BIT);
  if (!s_ymap) s_ymap = (uint16_t*)heap_caps_malloc(tgtH * sizeof(uint16_t), MALLOC_CAP_8BIT);

  // Allocate line buffer
  if (maxW > s_lineCap) {
    free(s_line16);
    s_line16 = (uint16_t*)heap_caps_malloc(maxW * sizeof(uint16_t),
                                           MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_lineCap = s_line16 ? maxW : 0;
  }

  // Build LUTs
  if (s_xmap) {
    for (int dx = 0; dx < s_dstW; ++dx) {
      s_xmap[dx] = (uint16_t)(roiX0 + ((int64_t)dx * roiW / s_dstW));
    }
  }
  if (s_ymap) {
    for (int dy = 0; dy < s_dstH; ++dy) {
      s_ymap[dy] = (uint16_t)(roiY0 + ((int64_t)dy * roiH / s_dstH));
    }
  }
}

// Render one frame following the PCE pattern
static inline void ws_render_one_frame()
{
  const uint16_t* fb = (const uint16_t*) FrameBuffer;
  if (!fb || !s_line16 || !s_xmap || !s_ymap) {
    static int s_nullCnt = 0;
    if ((s_nullCnt++ & 0xFF) == 0)
      printf("[WS-DISP] render skip: fb=%p line=%p xmap=%p ymap=%p\n",
             fb, s_line16, s_xmap, s_ymap);
    return;
  }

  // Recompute scaler if zoom or fullscreen changed
  if (ws_zoomPercent != lastZoomPercent || ws_fullscreen != lastFullscreen) {
    ws_display_compute_scaler();
    lastZoomPercent = ws_zoomPercent;
    lastFullscreen = ws_fullscreen;
  }

  const int dstW = s_dstW;
  const int dstH = s_dstH;
  if (dstW <= 0 || dstH <= 0) return;

  const bool ext   = s_use_ext;
  const bool use12 = s_use_12bit;

  static int s_frameCnt = 0;
  if (ext && s_frameCnt < 3) {
    printf("[WS-DISP] frame#%d ext=%d 12b=%d dstW=%d dstH=%d fb[0]=%04X stride=%d\n",
           s_frameCnt, ext, use12, dstW, dstH, fb[0], ws_fb_stride());
    s_frameCnt++;
  }

  // External-TFT path only: snapshot ~60% of bottom framebuffer
  // to reduce tearing. Internal LCD keeps the original direct path.
  if (ext) {
    int cacheLines = (kSrcH * 60 + 99) / 100;
    if (cacheLines < 1) cacheLines = 1;
    int cacheStartY = kSrcH - cacheLines;
    if (cacheStartY < 0) cacheStartY = 0;
    int neededWords = cacheLines * kSrcW;

    if (!s_bottomCache || neededWords > (s_bottomCacheLines * s_bottomCacheW)) {
      free(s_bottomCache);
      s_bottomCache = (uint16_t*)heap_caps_malloc(neededWords * sizeof(uint16_t), MALLOC_CAP_8BIT);
      if (s_bottomCache) {
        s_bottomCacheLines = cacheLines;
        s_bottomCacheW     = kSrcW;
      }
    }
    s_bottomCacheStartY = cacheStartY;

    if (s_bottomCache) {
      for (int i = 0; i < cacheLines; ++i) {
        int ySrc = cacheStartY + i;
        if (ySrc >= kSrcH) break;
        memcpy(s_bottomCache + i * kSrcW,
               fb + ySrc * ws_fb_stride(),
               kSrcW * sizeof(uint16_t));
      }
    }
  }

  if (ext) {
    s_tft.startWrite();
    // Re-assert COLMOD every frame (safety net)
    if (use12) {
      s_tft.writecommand(0x3A);
      s_tft.writedata(0x53);
    }
    s_tft.setAddrWindow(s_offX, s_offY, dstW, dstH);
  } else {
    M5Cardputer.Display.startWrite();
    M5Cardputer.Display.setAddrWindow(s_offX, s_offY, dstW, dstH);
  }

  int last_sy = -1;
  const int byteCount12 = ((dstW + 1) / 2) * 3;

  for (int dy = 0; dy < dstH; ++dy) {
    const int sy = s_ymap[dy];

    // Always rebuild when 12-bit: in-place conversion destroys lineBuf
    if (sy != last_sy || use12) {
      const uint16_t* srcLine = nullptr;

      // Use bottom cache if available
      if (s_bottomCache && sy >= s_bottomCacheStartY) {
        int idx = sy - s_bottomCacheStartY;
        if (idx >= 0 && idx < s_bottomCacheLines) {
          srcLine = s_bottomCache + idx * s_bottomCacheW;
        }
      }
      if (!srcLine) {
        srcLine = fb + sy * ws_fb_stride();
      }

      // Fill line buffer from the RGB565 framebuffer.
      int dx = 0;
      for (; dx + 8 <= dstW; dx += 8) {
        s_line16[dx + 0] = srcLine[s_xmap[dx + 0]];
        s_line16[dx + 1] = srcLine[s_xmap[dx + 1]];
        s_line16[dx + 2] = srcLine[s_xmap[dx + 2]];
        s_line16[dx + 3] = srcLine[s_xmap[dx + 3]];
        s_line16[dx + 4] = srcLine[s_xmap[dx + 4]];
        s_line16[dx + 5] = srcLine[s_xmap[dx + 5]];
        s_line16[dx + 6] = srcLine[s_xmap[dx + 6]];
        s_line16[dx + 7] = srcLine[s_xmap[dx + 7]];
      }
      for (; dx < dstW; ++dx) {
        s_line16[dx] = srcLine[s_xmap[dx]];
      }
      last_sy = sy;
    }

    if (ext && use12) {
      // RGB444 packed: convert RGB565 line -> 2 pixels -> 3 bytes.
      uint8_t *buf12 = (uint8_t*)s_line16;
      int pairs = dstW / 2;
      for (int p = 0; p < pairs; p++) {
        uint16_t c1 = s_line16[p * 2];
        uint16_t c2 = s_line16[p * 2 + 1];
        int j = p * 3;
        buf12[j]   = (uint8_t)(((c1 >> 8) & 0xF0) | ((c1 >> 7) & 0x0F));
        buf12[j+1] = (uint8_t)(((c1 << 3) & 0xF0) | ((c2 >> 12) & 0x0F));
        buf12[j+2] = (uint8_t)(((c2 >> 3) & 0xF0) | ((c2 >> 1) & 0x0F));
      }
      if (dstW & 1) {
        uint16_t c = s_line16[dstW - 1];
        int j = pairs * 3;
        buf12[j]   = (uint8_t)(((c >> 8) & 0xF0) | ((c >> 7) & 0x0F));
        buf12[j+1] = (uint8_t)((c << 3) & 0xF0);
        buf12[j+2] = 0;
      }
      s_tft.pushColors((uint16_t*)buf12, (byteCount12 + 1) / 2, false);
    } else if (ext) {
      s_tft.pushColors(s_line16, dstW, true);  // swap=true: LE -> SPI byte order
    } else {
      M5Cardputer.Display.writePixels(s_line16, dstW, true);
    }
  }

  if (ext) {
    s_tft.endWrite();
  } else {
    M5Cardputer.Display.endWrite();
  }
}

// Task
static void ws_display_task(void* arg)
{
  (void)arg;

  ws_display_compute_scaler();

  if (!s_use_ext) {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setSwapBytes(true);
    M5Cardputer.Display.fillScreen(TFT_BLACK);
  }

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ws_render_one_frame();
    if ((xTaskGetTickCount() & 7) == 0) taskYIELD();
  }
}

// API
extern "C" void ws_display_init(void)
{
  s_use_ext   = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
  s_use_12bit = s_use_ext && (g_emu_color_depth == EMU_COLOR_12BIT);
  printf("[WS-DISP] init: ext=%d 12bit=%d target=%d depth=%d\n",
         s_use_ext, s_use_12bit, g_emu_display_target, g_emu_color_depth);

  if (s_use_ext) {
    s_tft.begin();
    s_tft.setRotation(3);
    s_tft.fillScreen(TFT_BLACK);
    if (s_use_12bit) {
      s_tft.startWrite();
      s_tft.writecommand(0x3A);  // COLMOD
      s_tft.writedata(0x53);     // DPI=16bit, DBI=12bit (RGB444)
      s_tft.endWrite();
    }
  }

  AllocateBuffers();   // buffers video core oswan
}

extern "C" void ws_display_start()
{
  if (s_wsDispTask) return;

  BaseType_t ok = xTaskCreatePinnedToCore(
      ws_display_task, "WSDisp",
      s_use_ext ? 4096 : 2048,
      nullptr,
      s_use_ext ? 3 : 5,
      &s_wsDispTask,
      s_use_ext ? 1 : 0);

  if (ok != pdPASS) {
    s_wsDispTask = nullptr;
    printf("[WS][ERR] display task create failed\n");
  }
}

extern "C" void ws_display_stop()
{
  if (s_wsDispTask) {
    vTaskDelete(s_wsDispTask);
    s_wsDispTask = nullptr;
  }
  ws_display_free_buffers();

  // Clear external TFT on quit
  if (s_use_ext) {
    s_tft.fillScreen(TFT_BLACK);
  }
}

// Hook oswan
extern "C" void ws_graphics_paint(void)
{
  if (!s_wsDispTask) return;
  static int s_paintCnt = 0;
  if (s_paintCnt < 3)
    printf("[WS-DISP] paint#%d task=%p\n", s_paintCnt++, s_wsDispTask);
  xTaskNotifyGive(s_wsDispTask);
}

// External info screen (shown when game plays on internal LCD)
static std::string ws_truncate(const char* text, size_t maxChars)
{
  if (!text) return "";
  std::string v(text);
  if (v.size() <= maxChars) return v;
  if (maxChars <= 3) return v.substr(0, maxChars);
  return v.substr(0, maxChars - 3) + "...";
}

static void ws_draw_key_badge(int x, int y, const std::string& key)
{
  const int bw = 34, bh = 18;
  s_tft.fillRoundRect(x, y, bw, bh, 4, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  s_tft.drawCentreString(key.c_str(), x + bw / 2, y + 1, 2);
  s_tft.drawRoundRect(x, y, bw, bh, 4, TFT_YELLOW);
}

void ws_display_show_external_info(const char* romTitle, bool isColor)
{
  s_tft.begin();
  s_tft.setRotation(3);
  s_tft.fillScreen(TFT_BLACK);
  s_tft.setTextWrap(false);

  s_tft.drawRoundRect(8, 8, EXT_W - 16, EXT_H - 16, 8, TFT_DARKGREY);

  std::string title = romTitle ? romTitle : "";
  const char* drawTitle = title.empty() ? "WONDERSWAN" : title.c_str();
  int titleFont = 4;
  int maxTitleW = EXT_W - 32;
  if (s_tft.textWidth(drawTitle, 4) > maxTitleW) {
    titleFont = 2;
    if (s_tft.textWidth(drawTitle, 2) > maxTitleW) {
      title = ws_truncate(romTitle, 36);
      drawTitle = title.c_str();
    }
  }
  s_tft.setTextColor(TFT_CYAN, TFT_BLACK);
  s_tft.drawCentreString(drawTitle, EXT_W / 2, 16, titleFont);

  s_tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  const char* sysName = isColor ? "BANDAI WONDERSWAN COLOR" : "BANDAI WONDERSWAN";
  s_tft.drawCentreString(sysName, EXT_W / 2, 46, 2);

  s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
  s_tft.drawCentreString("VIDEO ON INTERNAL LCD", EXT_W / 2, 64, 2);

  s_tft.drawRoundRect(12, 86, EXT_W - 24, 98, 6, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft.drawCentreString("CONTROLS", EXT_W / 2, 92, 2);

  const auto actions = share::emuControlActionLabels(share::EmuProfile::Ws);
  const auto keys    = share::emuControlKeyLabels(share::EmuProfile::Ws);
  const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
  const size_t rowsPerCol = 4;
  const int threeColLabelX[3] = {22, 118, 214};
  const int twoColLabelX[2] = {34, 156};
  const int badgeOffset = 44;
  const int rowStep = 16;
  const int colCount = (count > rowsPerCol) ? (int)((count + rowsPerCol - 1) / rowsPerCol) : 1;

  for (size_t i = 0; i < count; ++i) {
    const int col = (int)(i / rowsPerCol);
    const int row = (int)(i % rowsPerCol);
    int baseX = 34;
    if (colCount >= 3) {
      baseX = threeColLabelX[(col < 3) ? col : 2];
    } else if (colCount == 2) {
      baseX = twoColLabelX[(col < 2) ? col : 1];
    }
    const int baseY = 112 + row * rowStep;

    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawString(actions[i].c_str(), baseX, baseY, 2);
    ws_draw_key_badge(baseX + badgeOffset, baseY - 3, keys[i]);
  }

  s_tft.drawFastHLine(18, 190, EXT_W - 36, TFT_DARKGREY);
  s_tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  s_tft.drawCentreString("GO / HOLD ESC = QUIT", EXT_W / 2, 198, 1);
  s_tft.drawCentreString("\\ = SCREEN   FN+,/ = ZOOM", EXT_W / 2, 210, 1);
}
