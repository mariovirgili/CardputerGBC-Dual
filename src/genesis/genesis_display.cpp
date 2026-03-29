#include "genesis_display.h"
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include "../tft_setup.h"
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include <string>

extern "C" {
  // Gwenesis VDP
  extern unsigned int screen_height;
}

// External TFT instance (file-scoped)
static TFT_eSPI s_tft;
static constexpr int EXT_W = 320;
static constexpr int EXT_H = 240;
static bool s_use_ext = false;
static bool s_use_12bit = false;

// Globals
QueueHandle_t g_scanQ = nullptr;
TaskHandle_t  g_displayTaskHandle = nullptr;
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
static uint16_t s_xmap[FB_W];
static int s_xmap_srcW  = -1, s_xmap_dstW = -1;
static int s_xmap_roiX0 = -1, s_xmap_roiW = -1;
static int s_roiX0 = 0, s_roiY0 = 0, s_roiW = 0, s_roiH = 0;

int genesisZoomPercent = 110;

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
  if (dstW <= 0 || dstW > FB_W) {
    printf("[MD-DISP] invalid xmap width %d\n", dstW);
    return false;
  }
  if (s_xmap &&
      s_xmap_srcW == srcW &&
      s_xmap_dstW == dstW &&
      s_xmap_roiX0 == roiX0 &&
      s_xmap_roiW  == roiW) {
    return true;
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
static inline bool allocate_line_buffers() {
  if (g_viewW > s_capImg) {
    uint16_t* newImg = (uint16_t*)heap_caps_malloc(g_viewW * sizeof(uint16_t),
                                                   MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!newImg) {
      printf("[MD-DISP] lineImg alloc failed for %d px\n", g_viewW);
      return false;
    }
    free(s_lineImg);
    s_lineImg = newImg;
    s_capImg  = g_viewW;
  }
  if (g_dstW > s_capFull) {
    uint16_t* newFull = (uint16_t*)heap_caps_malloc(g_dstW * sizeof(uint16_t),
                                                    MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!newFull) {
      printf("[MD-DISP] lineFull alloc failed for %d px\n", g_dstW);
      return false;
    }
    free(s_lineFull);
    s_lineFull = newFull;
    s_capFull  = g_dstW;
  }
  return s_lineImg && s_lineFull;
}

/* Initialize display subsystem */
extern "C" void genesis_display_init(void) {
  s_use_ext   = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
  s_use_12bit = s_use_ext && (g_emu_color_depth == EMU_COLOR_12BIT);

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
    g_dstW = EXT_W;   // 320
    g_dstH = EXT_H;   // 240
  } else {
    g_dstW = M5.Lcd.width();   // 240
    g_dstH = M5.Lcd.height();  // 135
  }

  // Screen viewport defaults
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
  if (!allocate_line_buffers()) {
    printf("[MD-DISP] initial line buffer allocation failed\n");
  }
  int prevDstY = 0;
  bool inFrame = false;
  int cachedSrcH = -1;
  int roiInitForSrcW = -1;
  const bool ext = s_use_ext;
  const bool use12 = s_use_12bit;

  for (;;) {
    ScanMsg m;
    if (xQueueReceive(g_scanQ, &m, portMAX_DELAY) != pdTRUE) continue;

    if (m.type == MSG_BEGIN_FRAME) {
      if (!allocate_line_buffers()) {
        if (inFrame) {
          if (ext) s_tft.endWrite();
          else M5.Lcd.endWrite();
          inFrame = false;
        }
        vTaskDelay(1);
        continue;
      }
      cachedSrcH = m.srcH;
      compute_centered_roi(/*srcW=*/FB_W, /*srcH=*/cachedSrcH);
      roiInitForSrcW = -1;

      if (ext) {
        s_tft.startWrite();
        // Re-assert COLMOD every frame (safety net — s_tft.begin() elsewhere resets to 16-bit)
        if (use12) {
          s_tft.writecommand(0x3A);
          s_tft.writedata(0x53);
        }
      } else {
        M5.Lcd.startWrite();
      }
      inFrame = true;
      prevDstY = g_viewY0;
      continue;
    }

    if (m.type == MSG_END_FRAME) {
      if (inFrame) {
        if (ext) {
          s_tft.endWrite();
        } else {
          M5.Lcd.endWrite();
        }
        inFrame = false;
      }
      continue;
    }

    if (roiInitForSrcW != (int)m.w) {
      compute_centered_roi((int)m.w, (int)m.srcH);
      if (!ensure_xmap_roi(/*srcW*/ m.w, /*dstW*/ g_viewW, /*roiX0*/ s_roiX0, /*roiW*/ s_roiW)) {
        vTaskDelay(1);
        continue;
      }
      roiInitForSrcW = (int)m.w;
    } else {
      if (!ensure_xmap_roi(/*srcW*/ m.w, /*dstW*/ g_viewW, /*roiX0*/ s_roiX0, /*roiW*/ s_roiW)) {
        vTaskDelay(1);
        continue;
      }
    }

    if (!s_lineImg || !s_lineFull) {
      vTaskDelay(1);
      continue;
    }

    if (m.w == g_viewW && s_roiX0 == 0 && s_roiW == m.w) {
      memcpy16(s_lineImg, m.data, g_viewW);
    } else {
      const uint16_t *xmap = s_xmap;
      for (int x = 0; x < g_viewW; ++x) {
        s_lineImg[x] = m.data[xmap[x]];
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

    if (ext && use12) {
      // 12-bit RGB444 path — convert s_lineFull in-place to packed bytes
      uint8_t *buf12 = (uint8_t*)s_lineFull;
      int pairs = g_dstW / 2;
      for (int p = 0; p < pairs; p++) {
        // Genesis VDP produces native LE RGB565
        uint16_t c1 = s_lineFull[p * 2];
        uint16_t c2 = s_lineFull[p * 2 + 1];
        int j = p * 3;
        buf12[j]   = ((c1 >> 8) & 0xF0) | ((c1 >> 7) & 0x0F);  // R1|G1
        buf12[j+1] = ((c1 << 3) & 0xF0) | ((c2 >> 12) & 0x0F); // B1|R2
        buf12[j+2] = ((c2 >> 3) & 0xF0) | ((c2 >> 1) & 0x0F);  // G2|B2
      }
      if (g_dstW & 1) {
        uint16_t c = s_lineFull[g_dstW - 1];
        int j = pairs * 3;
        buf12[j]   = ((c >> 8) & 0xF0) | ((c >> 7) & 0x0F);
        buf12[j+1] = ((c << 3) & 0xF0);
        buf12[j+2] = 0;
      }
      int byteCount = ((g_dstW + 1) / 2) * 3;
      int wordCount = (byteCount + 1) / 2;

      while (linesToPush > 0) {
        s_tft.setAddrWindow(0, prevDstY, g_dstW, 1);
        s_tft.pushColors((uint16_t*)buf12, wordCount, false);
        prevDstY++;
        linesToPush--;
        if ((prevDstY & 31) == 0) vTaskDelay(0);
      }
    } else {
      while (linesToPush > 0) {
        int chunk = (linesToPush > 16) ? 16 : linesToPush;
        if (ext) {
          s_tft.setAddrWindow(0, prevDstY, g_dstW, chunk);
          for (int i = 0; i < chunk; ++i) {
            s_tft.setAddrWindow(0, prevDstY + i, g_dstW, 1);
            s_tft.pushColors(s_lineFull, g_dstW, true);  // swap=true: LE → SPI byte order
          }
        } else {
          M5.Lcd.setAddrWindow(0, prevDstY, g_dstW, chunk);
          for (int i = 0; i < chunk; ++i) {
            M5.Lcd.pushPixels(s_lineFull, g_dstW);
          }
        }
        prevDstY    += chunk;
        linesToPush -= chunk;
        if ((prevDstY & 31) == 0) vTaskDelay(0);
      }
    }

    // Yield
    if ((m.line & 31) == 31) vTaskDelay(0);
  }
}

/* Start the display task */
extern "C" void genesis_display_start(void) {
  if (!g_scanQ) {
    g_scanQ = xQueueCreate(SCANLINE_QUEUE_DEPTH, sizeof(ScanMsg));
    if (!g_scanQ) {
      printf("[DISPLAY] queue create failed\n");
      return;
    }
  }
  if (!g_displayTaskHandle) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      display_task, "DisplayTask",
      4096, nullptr, 6, &g_displayTaskHandle,
      0 /* core  */
    );
    if (ok != pdPASS) {
      printf("[DISPLAY] task create failed\n");
      vQueueDelete(g_scanQ); g_scanQ = nullptr;
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
}

/* Begin a new frame */
extern "C" void genesis_display_begin_frame(uint16_t srcH) {
  if (!g_scanQ) return;
  // Reset cached ROI
  s_roiX0 = 0; s_roiY0 = 0; s_roiW = FB_W; s_roiH = srcH;
  ScanMsg b = { MSG_BEGIN_FRAME, 0, (uint16_t)FB_W, srcH, {0} };
  xQueueSend(g_scanQ, &b, portMAX_DELAY);
}

/* Gwenesis push scanline */
extern "C" void IRAM_ATTR GWENESIS_PUSH_SCANLINE(int line, const uint16_t* src16, int w) {
  if (!g_scanQ) return;

  ScanMsg m;
  m.type = MSG_SCANLINE;
  m.line = (uint16_t)line;
  m.w    = (uint16_t)w;

  int srcH = (g_srcH_cached > 0) ? g_srcH_cached
                                 : (int)(screen_height ? screen_height : 224);
  m.srcH = (uint16_t)srcH;

  // Copie ligne
  int copyW = (w < FB_W) ? w : FB_W;
  memcpy(m.data, src16, copyW * sizeof(uint16_t));
  if (copyW < FB_W) memset(m.data + copyW, 0, (FB_W - copyW) * sizeof(uint16_t));

  xQueueSend(g_scanQ, &m, portMAX_DELAY);
}

/* End the current frame */
extern "C" void genesis_display_end_frame(void) {
  if (!g_scanQ) return;
  uint16_t h = (uint16_t)(screen_height ? screen_height : 224u);
  ScanMsg e = { MSG_END_FRAME, 0, (uint16_t)FB_W, h, {0} };
  xQueueSend(g_scanQ, &e, portMAX_DELAY);
}

// ================== EXTERNAL INFO SCREEN ==================

static std::string genesis_truncate(const char* text, size_t maxChars)
{
  if (!text) return "";
  std::string v(text);
  if (v.size() <= maxChars) return v;
  if (maxChars <= 3) return v.substr(0, maxChars);
  return v.substr(0, maxChars - 3) + "...";
}

static void genesis_draw_key_badge(int x, int y, const std::string& key)
{
  const int bw = 34, bh = 18;
  s_tft.fillRoundRect(x, y, bw, bh, 4, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  s_tft.drawCentreString(key.c_str(), x + bw / 2, y + 1, 2);
  s_tft.drawRoundRect(x, y, bw, bh, 4, TFT_YELLOW);
}

void genesis_display_show_external_info(const char* romTitle)
{
  s_tft.begin();
  s_tft.setRotation(3);
  s_tft.fillScreen(TFT_BLACK);
  s_tft.setTextWrap(false);

  s_tft.drawRoundRect(8, 8, EXT_W - 16, EXT_H - 16, 8, TFT_DARKGREY);

  std::string title = romTitle ? romTitle : "";
  const char* drawTitle = title.empty() ? "GENESIS" : title.c_str();
  int titleFont = 4;
  int maxTitleW = EXT_W - 32;
  if (s_tft.textWidth(drawTitle, 4) > maxTitleW) {
    titleFont = 2;
    if (s_tft.textWidth(drawTitle, 2) > maxTitleW) {
      title = genesis_truncate(romTitle, 36);
      drawTitle = title.c_str();
    }
  }
  s_tft.setTextColor(TFT_CYAN, TFT_BLACK);
  s_tft.drawCentreString(drawTitle, EXT_W / 2, 16, titleFont);

  s_tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  s_tft.drawCentreString("SEGA MEGA DRIVE / GENESIS", EXT_W / 2, 46, 2);

  s_tft.setTextColor(TFT_GREEN, TFT_BLACK);
  s_tft.drawCentreString("VIDEO ON INTERNAL LCD", EXT_W / 2, 64, 2);

  s_tft.drawRoundRect(12, 86, EXT_W - 24, 98, 6, TFT_DARKGREY);
  s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
  s_tft.drawCentreString("CONTROLS", EXT_W / 2, 92, 2);

  const auto actions = share::emuControlActionLabels(share::EmuProfile::Genesis);
  const auto keys    = share::emuControlKeyLabels(share::EmuProfile::Genesis);
  const size_t count = (actions.size() < keys.size()) ? actions.size() : keys.size();
  const size_t rowsPerCol = 4;

  for (size_t i = 0; i < count; ++i) {
    const int col = (int)(i / rowsPerCol);
    const int row = (int)(i % rowsPerCol);
    const int baseX = 24 + col * 146;
    const int baseY = 112 + row * 16;

    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.drawString(actions[i].c_str(), baseX, baseY, 2);
    genesis_draw_key_badge(baseX + 88, baseY - 3, keys[i]);
  }

  s_tft.drawFastHLine(18, 190, EXT_W - 36, TFT_DARKGREY);
  s_tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  s_tft.drawCentreString("GO / HOLD ESC = QUIT", EXT_W / 2, 198, 1);
  s_tft.drawCentreString("\\ = SCREEN  FN+,/ = ZOOM", EXT_W / 2, 210, 1);
}
