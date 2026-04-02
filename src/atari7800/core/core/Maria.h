/* ----------------------------------------------------------------------------
 *   ___  ___  ___  ___       ___  ____  ___  _  _
 *  /__/ /__/ /  / /__  /__/ /__    /   /_   / |/ /
 * /    / \  /__/ ___/ ___/ ___/   /   /__  /    /  emulator
 *
 * ----------------------------------------------------------------------------
 * Copyright 2005 Greg Stanton
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * ----------------------------------------------------------------------------
 * Maria.h
 * ----------------------------------------------------------------------------
 */
#ifndef MARIA_H
#define MARIA_H

/* Surface sizes: 320 * visible area height */
#define MARIA_SURFACE_ROW_WIDTH 320
#define MARIA_SURFACE_ROWS_MAX 272
#define MARIA_SURFACE_SLAB_ROWS_TARGET 128
#define MARIA_SURFACE_SLAB_ROWS_MIN 8
#define MARIA_SURFACE_SLABS_MAX ((MARIA_SURFACE_ROWS_MAX + MARIA_SURFACE_SLAB_ROWS_MIN - 1) / MARIA_SURFACE_SLAB_ROWS_MIN)
#define MARIA_SURFACE_SIZE_NTSC (223 * 320)   /* 71360 */
#define MARIA_SURFACE_SIZE_PAL  (272 * 320)   /* 87040 */
#define MARIA_SURFACE_SIZE      93440         /* max, for non-ESP builds */

#include <stdint.h>
#include <boolean.h>
#include "Rect.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t maria_surface_size;
extern bool maria_EnsureAllocated(void);
extern bool maria_IsReady(void);
extern void maria_Shutdown(void);
extern void maria_Reset(void);
extern uint32_t maria_RenderScanline(void);
extern void maria_Clear(void);

extern rect maria_displayArea;
extern rect maria_visibleArea;
extern uint8_t* maria_surface;
extern uint8_t* maria_surface_rows[MARIA_SURFACE_ROWS_MAX];
extern bool maria_surface_segmented;
extern uint16_t maria_surface_top_crop;
extern uint16_t maria_surface_height;
extern uint16_t maria_scanline;
/* Set true for frames whose output will be discarded (frameskip).
 * DMA cycle counts and memory reads still run normally; only the pixel
 * writes to maria_lineRAM and maria_surface are suppressed. */
extern bool maria_skip_render;

#ifdef __cplusplus
}
#endif

#endif



