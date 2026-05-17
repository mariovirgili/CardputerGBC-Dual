#pragma GCC optimize ("Ofast")

#include "gx4000_display.h"
#include <M5Cardputer.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "share/emu_log_cpp.h"

// ─────────────────────────────────────────────────────────────────────────────
// Runtime state
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t *s_lineBuf   = nullptr;  // GX4000_LCD_W pixels, 16-bit RGB565
static int       s_prevLcdY  = -1;       // last LCD row pushed (for skip logic)
static int       s_xOff      = 0;        // horizontal offset on LCD (centering)
static int       s_yOff      = 0;        // vertical offset on LCD (centering)
static int       s_dstW      = GX4000_LCD_W;
static int       s_dstH      = GX4000_LCD_H;
static int       s_roiX0     = 0;
static int       s_roiY0     = 0;
static int       s_roiW      = GX4000_CPC_VISIBLE_W;
static int       s_roiH      = GX4000_CPC_VISIBLE_H;
static bool      s_fullscreen = true;
static int       s_zoomPercent = 120;

static bool      s_frameOpen = false;    // display in startWrite/endWrite block

typedef struct {
    int lcdY;
    int bufIndex;
} GX4000LineMsg;

static constexpr int kLinePoolCount = 8;
static uint16_t *s_linePool = nullptr;   // kLinePoolCount * GX4000_LCD_W pixels
static QueueHandle_t s_lineFreeQ = nullptr;
static QueueHandle_t s_lineReadyQ = nullptr;
static TaskHandle_t s_displayTask = nullptr;
static volatile bool s_displayRunning = false;
static volatile uint32_t s_dropLines = 0;
static uint16_t *s_srcXMap = nullptr;
static int s_srcMapRenderW = -1;

static inline int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void gx4000_display_recompute_view(void)
{
    const int srcW = GX4000_CPC_VISIBLE_W;
    const int srcH = GX4000_CPC_VISIBLE_H;

    if (s_fullscreen) {
        s_dstW = GX4000_LCD_W;
        s_dstH = GX4000_LCD_H;
        s_xOff = 0;
        s_yOff = 0;
    } else {
        // Keep CPC aspect ratio in a centered viewport.
        int fitW = (GX4000_LCD_H * srcW) / srcH;
        fitW = clampi(fitW, 1, GX4000_LCD_W);
        s_dstW = fitW;
        s_dstH = GX4000_LCD_H;
        s_xOff = (GX4000_LCD_W - s_dstW) / 2;
        s_yOff = 0;
    }

    int zp = (s_zoomPercent <= 0) ? 100 : s_zoomPercent;
    int roiW = (srcW * 100) / zp;
    int roiH = (srcH * 100) / zp;
    roiW = clampi(roiW, 32, srcW);
    roiH = clampi(roiH, 32, srcH);

    s_roiW = roiW;
    s_roiH = roiH;
    s_roiX0 = (srcW - roiW) / 2;
    s_roiY0 = (srcH - roiH) / 2;

    s_prevLcdY = -1;
    s_srcMapRenderW = -1;
}

static void gx4000_display_task(void *arg)
{
    (void)arg;
    GX4000LineMsg msg;
    M5Cardputer.Display.startWrite();

    while (s_displayRunning) {
        // Block until the first line of a batch is ready
        if (xQueueReceive(s_lineReadyQ, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }

        // Hold the SPI bus for the entire batch

        do {
            if (s_linePool && msg.bufIndex >= 0 && msg.bufIndex < kLinePoolCount) {
                uint16_t *line = s_linePool + (msg.bufIndex * GX4000_LCD_W);
                M5Cardputer.Display.setAddrWindow(s_xOff, s_yOff + msg.lcdY,
                                                  GX4000_LCD_W, 1);
                M5Cardputer.Display.writePixels(line, GX4000_LCD_W);
                int idx = msg.bufIndex;
                (void)xQueueSend(s_lineFreeQ, &idx, 0);
            }
        } while (xQueueReceive(s_lineReadyQ, &msg, 0) == pdTRUE);

    }
    
    M5Cardputer.Display.endWrite();
    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
void gx4000_display_init(void)
{
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setSwapBytes(true);    // let driver handle endian swap
    M5Cardputer.Display.fillScreen(TFT_BLACK);

    // Allocate line buffer aligned for dma
    s_lineBuf = (uint16_t *)heap_caps_malloc(
        GX4000_LCD_W * sizeof(uint16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (!s_lineBuf) {
        EMU_LOG("[GX4000] ERROR: line buffer alloc failed\n");
    }

    // Calculate centering: CPC aspect is 4:3, letterbox to LCD
    // Destination size = fit GX4000_CPC_VISIBLE_W × GX4000_CPC_VISIBLE_H
    //   into GX4000_LCD_W × GX4000_LCD_H keeping aspect
    int lcdW = M5Cardputer.Display.width();
    int lcdH = M5Cardputer.Display.height();
    // Simple nearest-neighbor scale: target = LCD w×h
    // Horizontal fills completely (384→240 = 5/8), vertical likewise (272→135)
    s_xOff = 0;
    s_yOff = 0;

    s_prevLcdY  = -1;
    s_frameOpen = false;
    s_dropLines = 0;
    gx4000_display_recompute_view();
    s_srcMapRenderW = -1;
    s_srcXMap = (uint16_t *)heap_caps_malloc(
        GX4000_LCD_W * sizeof(uint16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    s_linePool = (uint16_t *)heap_caps_malloc(
        (size_t)kLinePoolCount * GX4000_LCD_W * sizeof(uint16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    s_lineFreeQ = xQueueCreate(kLinePoolCount, sizeof(int));
    s_lineReadyQ = xQueueCreate(kLinePoolCount, sizeof(GX4000LineMsg));
    if (s_linePool && s_lineFreeQ && s_lineReadyQ) {
        for (int i = 0; i < kLinePoolCount; ++i) {
            (void)xQueueSend(s_lineFreeQ, &i, 0);
        }
        s_displayRunning = true;
        BaseType_t ok = xTaskCreatePinnedToCore(
            gx4000_display_task,
            "gx4_disp",
            3072,
            nullptr,
            8,
            &s_displayTask,
            0);
        if (ok != pdPASS) {
            s_displayTask = nullptr;
            s_displayRunning = false;
        }
    }

    EMU_LOG("[GX4000] display init: lcd=%dx%d linebuf=%p\n",
           lcdW, lcdH, s_lineBuf);
}

// ─────────────────────────────────────────────────────────────────────────────
void gx4000_display_shutdown(void)
{
    if (s_frameOpen) {
        M5Cardputer.Display.endWrite();
        s_frameOpen = false;
    }
    if (s_lineBuf) {
        heap_caps_free(s_lineBuf);
        s_lineBuf = nullptr;
    }

    s_displayRunning = false;
    if (s_displayTask) {
        vTaskDelay(pdMS_TO_TICKS(10));
        s_displayTask = nullptr;
    }
    if (s_lineReadyQ) {
        vQueueDelete(s_lineReadyQ);
        s_lineReadyQ = nullptr;
    }
    if (s_lineFreeQ) {
        vQueueDelete(s_lineFreeQ);
        s_lineFreeQ = nullptr;
    }
    if (s_linePool) {
        heap_caps_free(s_linePool);
        s_linePool = nullptr;
    }
    if (s_srcXMap) {
        heap_caps_free(s_srcXMap);
        s_srcXMap = nullptr;
    }

    s_prevLcdY = -1;
}

void gx4000_display_flush_line(int visY,
                                const unsigned char *data,
                                int x_offset,
                                int render_w,
                                int bytes_per_pixel)
{
    if (!s_lineBuf || !data) return;

    // Crop vertically to ROI first.
    if (visY < s_roiY0 || visY >= (s_roiY0 + s_roiH)) return;

    // Vertical decimation ROI to destination viewport
    int relY = visY - s_roiY0;
    int lcdY = s_yOff + (int)(((unsigned)relY * (unsigned)s_dstH) / (unsigned)s_roiH);
    if (lcdY < s_yOff || lcdY >= (s_yOff + s_dstH)) return;
    // Skip duplicate mapped rows (prevents horizontal offset artifacts)
    if (lcdY == s_prevLcdY) return;
    s_prevLcdY = lcdY;

    // Source pointer: skip x_offset pixels
    const uint8_t *src = data + (x_offset * bytes_per_pixel);

    uint16_t *dstLine = nullptr;
    int pooledIndex = -1;
    if (s_displayRunning && s_linePool && s_lineFreeQ && s_lineReadyQ) {
        if (xQueueReceive(s_lineFreeQ, &pooledIndex, 0) == pdTRUE) {
            dstLine = s_linePool + (pooledIndex * GX4000_LCD_W);
        }
    }

    /* Never block emulation with synchronous LCD I/O
       If no free line buffer is available, just drop this line */
    if (!dstLine || pooledIndex < 0) {
        s_dropLines = s_dropLines + 1;
        return;
    }

    if (bytes_per_pixel == 2) {
        if (s_srcXMap && render_w != s_srcMapRenderW) {
            s_srcMapRenderW = render_w;
            for (int x = 0; x < s_dstW; ++x) {
                int srcX = s_roiX0 + (int)(((unsigned)x * (unsigned)s_roiW) / (unsigned)s_dstW);
                if (srcX >= render_w) srcX = render_w - 1;
                s_srcXMap[x] = (uint16_t)srcX;
            }
        }

        // 16-bpp line already in RGB565, scale horizontally
        const uint16_t *src16 = (const uint16_t *)src;
        if (s_xOff > 0) {
            memset(dstLine, 0, (size_t)s_xOff * sizeof(uint16_t));
        }

        if (s_srcXMap) {
            for (int x = 0; x < s_dstW; ++x) {
                dstLine[s_xOff + x] = src16[s_srcXMap[x]];
            }
        } else {
            for (int x = 0; x < s_dstW; ++x) {
                int srcX = s_roiX0 + (int)(((unsigned)x * (unsigned)s_roiW) / (unsigned)s_dstW);
                if (srcX >= render_w) srcX = render_w - 1;
                dstLine[s_xOff + x] = src16[srcX];
            }
        }

        if ((s_xOff + s_dstW) < GX4000_LCD_W) {
            int right = GX4000_LCD_W - (s_xOff + s_dstW);
            memset(dstLine + s_xOff + s_dstW, 0, (size_t)right * sizeof(uint16_t));
        }
    } else {
        // 8-bpp paletted, must not use this path in TrueColour mode
        // Fill with black as fallback
        memset(dstLine, 0, GX4000_LCD_W * sizeof(uint16_t));
    }

    if (s_displayRunning && s_lineReadyQ) {
        GX4000LineMsg msg = { lcdY, pooledIndex };
        if (xQueueSend(s_lineReadyQ, &msg, 0) != pdTRUE) {
            int idx = pooledIndex;
            (void)xQueueSend(s_lineFreeQ, &idx, 0);
            s_dropLines = s_dropLines + 1;
        }
    }
}

void gx4000_display_frame_done(void)
{
    s_prevLcdY = -1;
}

void gx4000_display_cycle_view(void)
{
    s_fullscreen = true;
    s_zoomPercent += 10;
    if (s_zoomPercent > 150) {
        s_zoomPercent = 100;
    }
    gx4000_display_recompute_view();
}

void gx4000_display_zoom_in(void)
{
    if (!s_fullscreen) s_fullscreen = true;
    s_zoomPercent = clampi(s_zoomPercent + 1, 100, 150);
    gx4000_display_recompute_view();
}

void gx4000_display_zoom_out(void)
{
    if (!s_fullscreen) s_fullscreen = true;
    s_zoomPercent = clampi(s_zoomPercent - 1, 100, 150);
    gx4000_display_recompute_view();
}

unsigned long gx4000_display_take_drop_count(void)
{
    uint32_t v = s_dropLines;
    s_dropLines = 0;
    return (unsigned long)v;
}
