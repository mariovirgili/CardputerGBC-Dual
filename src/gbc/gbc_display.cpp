#include "gbc_display.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
// Include the dual screen setup
#include "../tft_setup.h"
#include "esp_heap_caps.h"
#include <math.h>

extern "C" {
  #include "gnuboy/gnuboy.h"
}

// Instance for external screen
TFT_eSPI tft = TFT_eSPI();

// Target resolution for ILI9341
static constexpr int LCD_W = 320;
static constexpr int LCD_H = 240;

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
static QueueHandle_t s_frameQ  = nullptr;
static TaskHandle_t  s_task    = nullptr;
static uint16_t     *s_lineBuf = nullptr;
static int           s_lineCap = 0;

// Cache of transform parameters to avoid recalculations
struct GbcDisplayTransform {
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

static GbcDisplayTransform s_transform;

static int  s_lastZoomPercent = -1;
static bool s_lastFullScreen  = false;
static int  s_lastSrcW        = 0;
static int  s_lastSrcH        = 0;
static int  s_lastLcdW        = 0;
static int  s_lastLcdH        = 0;

static void gbc_display_transform(int srcW, int srcH)
{
  // Use TFT dimensions
  int lcdW = LCD_W;
  int lcdH = LCD_H;

  if (lcdW <= 0 || lcdH <= 0 || srcW <= 0 || srcH <= 0) {
    return;
  }

  bool full = gbcFullScreen;
  int  zoom = (gbcZoomPercent > 0) ? gbcZoomPercent : 100;

  // nothing changed
  if (zoom          == s_lastZoomPercent &&
      full          == s_lastFullScreen  &&
      srcW          == s_lastSrcW        &&
      srcH          == s_lastSrcH        &&
      lcdW          == s_lastLcdW        &&
      lcdH          == s_lastLcdH) {
    return;
  }

  if (!full) {
    tft.fillScreen(TFT_BLACK);
  }

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
    // “original” mode, aspect ratio preserved
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
  } else {
    // fullscreen (stretched)
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

    int srcW  = msg.width;
    int srcH  = msg.height;
    int pitch = msg.pitch;

    gbc_display_transform(srcW, srcH);

    int   dstW      = s_transform.dstW;
    int   dstH      = s_transform.dstH;
    int   xOffset   = s_transform.xOffset;
    int   yOffset   = s_transform.yOffset;
    float invScaleX = s_transform.invScaleX;
    float invScaleY = s_transform.invScaleY;
    float srcCX     = s_transform.srcCX;
    float srcCY     = s_transform.srcCY;
    float dstCX     = s_transform.dstCX;
    float dstCY     = s_transform.dstCY;

    if (dstW <= 0 || dstH <= 0) {
      continue;
    }

    // (re)allocation of the RGB565 line buffer
    if (dstW > s_lineCap) {
      if (s_lineBuf) {
        free(s_lineBuf);
        s_lineBuf = nullptr;
      }
      s_lineBuf = (uint16_t*)heap_caps_malloc(
          dstW * sizeof(uint16_t),
          MALLOC_CAP_DMA | MALLOC_CAP_8BIT
      );
      s_lineCap = dstW;
    }
    if (!s_lineBuf) {
      continue;
    }

    // Use TFT_eSPI for rendering
    tft.startWrite();

    for (int y = 0; y < dstH; ++y) {
      float fy    = (float)y - dstCY;
      float srcYf = srcCY + fy * invScaleY; // Optimized simplified math
      int   srcY  = (int)srcYf;
      if (srcY < 0)      srcY = 0;
      if (srcY >= srcH)  srcY = srcH - 1;

      const uint16_t *srcLine = msg.fb + srcY * pitch;

      for (int x = 0; x < dstW; ++x) {
        float srcXf = srcCX + ( (float)x - dstCX ) * invScaleX;
        int   srcX  = (int)srcXf;
        if (srcX < 0)      srcX = 0;
        if (srcX >= srcW)  srcX = srcW - 1;

        s_lineBuf[x] = srcLine[srcX];
      }

      int dstY = yOffset + y;
      tft.setAddrWindow(xOffset, dstY, dstW, 1);
      tft.pushColors(s_lineBuf, dstW);
    }

    tft.endWrite();
    vTaskDelay(0);
  }
}

// ================== PUBLIC API ==================

extern "C" void gbc_display_init(void)
{
  // Initialize External TFT
  tft.begin();
  tft.setRotation(3); // Landscape
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);

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
      2048,        // stack
      nullptr,
      5,           // priority
      &s_task,
      1            // core 1 to avoid interference with emu on core 0 if possible
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
  if (s_lineBuf) {
    free(s_lineBuf);
    s_lineBuf = nullptr;
    s_lineCap = 0;
  }
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

  // non blocking
  BaseType_t ok = xQueueSend(s_frameQ, &msg, 0);
  (void)ok;
}