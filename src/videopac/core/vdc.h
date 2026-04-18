#ifndef __VDC_H
#define __VDC_H

#include <stdint.h>

#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
#define BORDERW 8
#define BMPW (160 + BORDERW)
#define BMPH 120
#define WNDW 160
#define WNDH 120
#else
#define BORDERW 20
#define BMPW 340
#define BMPH 250
#define WNDW 320
#define WNDH 240
#endif
/*********/
#define BOX_W     MIN(512, SCREEN_W-16)
#define BOX_H     MIN(256, (SCREEN_H-64)&0xFFF0)

#define BOX_L     ((SCREEN_W - BOX_W) / 2)
#define BOX_R     ((SCREEN_W + BOX_W) / 2)
#define BOX_T     ((SCREEN_H - BOX_H) / 2)
#define BOX_B     ((SCREEN_H + BOX_H) / 2)

/*********/
extern uint8_t coltab[];
extern long clip_low;
extern long clip_high;

const uint8_t* o2em_vdc_get_framebuffer(int* out_width, int* out_height, int* out_pitch);
void o2em_vdc_get_palette565(uint16_t out_palette[256]);

void init_display(void);
void draw_display(void);
void set_textmode(void);
void draw_region(void);
void finish_display();
void close_display(void);
void grmode(void);
void display_bg(void);
void clear_collision(void);
void clearscr(void);

#endif


