#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define FB_W 320
#ifndef SCANLINE_QUEUE_DEPTH
#define SCANLINE_QUEUE_DEPTH 16
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCANMSG_FORMAT_RGB565 0
#define SCANMSG_FORMAT_INDEX8 1

typedef enum : uint8_t {
    MSG_BEGIN_FRAME,
    MSG_SCANLINE,
    MSG_END_FRAME
} ScanMsgType;

typedef struct {
    uint8_t  idx[FB_W];      // 8-bit palette indexes for one scanline
    uint16_t palette[64];    // RGB565 palette snapshot for this scanline
} ScanMsgIndexLine;

typedef struct {
    ScanMsgType type;
    uint16_t    line;      // pour MSG_SCANLINE
    uint16_t    w;         // largeur utile
    uint16_t    srcH;      // hauteur source
    uint8_t     format;    // SCANMSG_FORMAT_*
    union {
        uint16_t         data[FB_W]; // RGB565 payload ligne
        ScanMsgIndexLine indexed;    // 8-bit indexes + per-line palette snapshot
    };
} ScanMsg;

// Shared globals
extern QueueHandle_t g_scanQ;
extern TaskHandle_t  g_displayTaskHandle;
extern int g_dstW, g_dstH;
extern int g_viewX0, g_viewY0;
extern int g_viewW, g_viewH;
extern bool g_inFrame;
extern int g_prevDstY;
extern int g_srcH_cached;
extern int g_field_ofs;
extern uint16_t *s_lineImg;
extern int s_capImg;

// Initialize display subsystem
void genesis_display_init(void);

// Create and start the display task and its queue
void genesis_display_start(void);

// Stop display task and free resources
void genesis_display_stop(void);

// Begin a new frame
void genesis_display_begin_frame(uint16_t srcH);

// Used by Gwenesis to push a scanline to the display task
void GWENESIS_PUSH_SCANLINE(int line, const uint16_t* src16, int w);
void GWENESIS_PUSH_SCANLINE_IDX(int line, const uint8_t* src8, int w);

// End the current frame
void genesis_display_end_frame(void);

// Display task
void display_task(void* arg);


#ifdef __cplusplus
}
#endif
