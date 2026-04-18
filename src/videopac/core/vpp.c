
/*
 *   O2EM Free Odyssey2 / Videopac+ Emulator
 *
 *   Created by Daniel Boris <dboris@comcast.net>  (c) 1997,1998
 *
 *   Developed by Andre de la Rocha   <adlroc@users.sourceforge.net>
 *             Arlindo M. de Oliveira <dgtec@users.sourceforge.net>
 *
 *   http://o2em.sourceforge.net
 *
 *
 *
 *   Videopac+ G7400 emulation
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(VIDEOPAC_VPP_DUAL_CORE)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif
#include "vmachine.h"
#include "vdc.h"
#include "vpp_cset.h"
#include "vpp.h"

#include "wrapalleg.h"
#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
#include "../../share/emu_static_pool.h"
#endif

static uint8_t LumReg = 0xff, TraReg = 0xff;
static ALLEGRO_BITMAP *vppbmp = NULL;
static uint8_t *colplus = NULL;
static int vppon = 1;
static int vpp_cx = 0;
static int vpp_cy = 0;
static uint8_t vpp_data = 0;
static int inc_curs=1;
static int slice=0;
static int vpp_y0=0;
static uint8_t vpp_r=0;
uint8_t dchars[2][960];
uint8_t vpp_mem[40][32][4];
static int frame_cnt=0;
static int blink_st=0;
static int slicemode=0;
static int need_update=0;

#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
typedef struct {
	uint8_t *vmem;
	int offx;
	int offy;
	int w;
	int h;
	int totw;
	int toth;
	int stop;
} VppRenderJob;

static uint8_t *vpp_low_cache = NULL;
static int vpp_low_cache_valid = 0;
static int vpp_low_cache_openb = -1;
static int vpp_external_native_mode = 0;
static int vpp_external_cache_valid = 0;
static int vpp_external_last_frame = -1;
static int vpp_external_openb = -1;
static int vpp_external_line_sy = -9999;
static int vpp_external_map_dst_w = -1;
static int vpp_external_map_dst_h = -1;
static int vpp_external_map_src_w = -1;
static int vpp_external_map_src_h = -1;
static uint8_t vpp_external_row_dirty[25];
static uint16_t vpp_external_base_xmap[320];
static int16_t vpp_external_overlay_xmap[320];
static int16_t vpp_external_sy_map[240];
static uint8_t vpp_external_line_cache[320];

#if EMU_STATIC_POOL_SIZE < (BMPW * BMPH * 3u)
#error "Videopac+ low-memory VPP cache does not fit in the shared emulator pool"
#endif

#if defined(VIDEOPAC_VPP_DUAL_CORE)
static TaskHandle_t vpp_task_handle = NULL;
static SemaphoreHandle_t vpp_job_sem = NULL;
static SemaphoreHandle_t vpp_done_sem = NULL;
static VppRenderJob vpp_job;
static int vpp_task_ready = 0;
#endif
#endif

static void vpp_mark_dirty(void)
{
	need_update = 1;
#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
	vpp_low_cache_valid = 0;
	vpp_external_cache_valid = 0;
	memset(vpp_external_row_dirty, 1, sizeof(vpp_external_row_dirty));
	vpp_external_line_sy = -9999;
#endif
}

#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
static int vpp_external_row_for_source_row(int source_row)
{
	if (source_row == 31)
		return 0;
	if (source_row >= 0 && source_row < 24)
		return ((source_row - vpp_y0 + 24) % 24) + 1;
	return -1;
}

static void vpp_mark_source_row_dirty(int source_row)
{
	const int row = vpp_external_row_for_source_row(source_row);

	need_update = 1;
	vpp_low_cache_valid = 0;
	vpp_external_line_sy = -9999;
	if (row >= 0)
		vpp_external_row_dirty[row] = 1;
	else
		vpp_mark_dirty();
}

#endif


uint8_t read_PB(uint8_t p){
	p &= 0x3;
	switch (p)
   {
      case 0:
         return LumReg >> 4;
      case 1:
         return LumReg & 0xf;
      case 2:
         return TraReg >> 4;
      case 3:
         return TraReg & 0xf;
   }
	return 0;
}


void write_PB(uint8_t p, uint8_t val){
	uint8_t old_lum = LumReg;
	uint8_t old_tra = TraReg;

	p &= 0x3;
	val &= 0xf;

	switch (p) {
		case 0:
			LumReg = (val<<4) | (LumReg & 0xf);
			break;
		case 1:
			LumReg = (LumReg & 0xf0) | val;
			break;
		case 2:
			TraReg = (val<<4) | (TraReg & 0xf);
			break;
		case 3:
			TraReg = (TraReg & 0xf0) | val;
			break;
	}	
	if (LumReg != old_lum || TraReg != old_tra)
		vpp_mark_dirty();
}


uint8_t vpp_read(uint16_t adr){
	uint8_t t;
	static uint8_t ta=0;
	static uint8_t tb=0;

	switch (adr){
		case 4:
			return ta;
		case 5:
			/* get return value from last read */
			t = tb;
			/* the real VPP starts a read cycle, 
			 * the data gets returned at next read */
			if (slicemode) {
				uint8_t ext, chr;
				chr = vpp_mem[vpp_cx][vpp_cy][0];
				ext = (vpp_mem[vpp_cx][vpp_cy][1] & 0x80) ? 1 : 0;
				if (chr < 0xA0) {
					ta = 0;
				} else {
					ta = dchars[ext][(chr-0xA0)*10+slice];
					ta = ((ta&0x80)>>7) | ((ta&0x40)>>5) | ((ta&0x20)>>3) | ((ta&0x10)>>1) | ((ta&0x08)<<1) | ((ta&0x04)<<3) | ((ta&0x02)<<5) | ((ta&0x01)<<7);
				}
				tb = 0xff; /* real VPP seems to return junk */
				slice = (slice+1) % 10;
			} else {
				ta = vpp_mem[vpp_cx][vpp_cy][1];
				tb = vpp_mem[vpp_cx][vpp_cy][0]; 
				if (inc_curs) {
					vpp_cx++;
					if (vpp_cx >= 40) {
						vpp_cx = 0;
						vpp_cy++;
						if (vpp_cy >= 24) vpp_cy = 0;
					}
				}
			}
			return t;
		case 6:
			return 0;
		default:
         break;
	}
   return 0;
}


void vpp_write(uint8_t dat, uint16_t adr){
	static uint8_t ta;
	int dirty = 0;
	int dirty_row0 = -1;
	int dirty_row1 = -1;

	switch (adr) {
		case 0:
			if (!slicemode) {
				if (vpp_mem[vpp_cx][vpp_cy][1] != dat)
					dirty_row0 = vpp_cy;
				vpp_mem[vpp_cx][vpp_cy][1] = dat;
			} else {
				ta = dat;
			}
			break;
		case 1:
			if (slicemode) {
				uint8_t ext, chr, bits;
				chr = vpp_mem[vpp_cx][vpp_cy][0];
				ext = (vpp_mem[vpp_cx][vpp_cy][1] & 0x80) ? 1 : 0;
				bits = ((ta&0x80)>>7) | ((ta&0x40)>>5) | ((ta&0x20)>>3) | ((ta&0x10)>>1) | ((ta&0x08)<<1) | ((ta&0x04)<<3) | ((ta&0x02)<<5) | ((ta&0x01)<<7);
				if (chr >= 0xA0) {
					uint8_t *dst = &dchars[ext][(chr-0xA0)*10+slice];
					if (*dst != bits)
						dirty = 1;
					*dst = bits;
				}
				slice = (slice+1) % 10;
			} else {
				const int old_cy = vpp_cy;
				uint8_t old_chr = vpp_mem[vpp_cx][vpp_cy][0];
				uint8_t old_ser_chr = vpp_mem[vpp_cx][vpp_cy][2];
				uint8_t old_ser_atr = vpp_mem[vpp_cx][vpp_cy][3];

				vpp_mem[vpp_cx][vpp_cy][0] = dat;
				if ((dat>0x7f) && (dat<0xa0) && (!(vpp_mem[vpp_cx][vpp_cy][1] & 0x80))) {
					vpp_mem[vpp_cx][vpp_cy][2] = dat;
					vpp_mem[vpp_cx][vpp_cy][3] = vpp_mem[vpp_cx][vpp_cy][1];
				} else {
					vpp_mem[vpp_cx][vpp_cy][2] = vpp_mem[vpp_cx][vpp_cy][3] = 0;
				}
				if (old_chr != vpp_mem[vpp_cx][vpp_cy][0] ||
				    old_ser_chr != vpp_mem[vpp_cx][vpp_cy][2] ||
				    old_ser_atr != vpp_mem[vpp_cx][vpp_cy][3])
					dirty_row0 = old_cy;
				if (inc_curs) {
					int old_cx = vpp_cx;
					vpp_cx++;
					if (vpp_cx >= 40) {
						vpp_cx = 0;
						vpp_cy++;
						if (vpp_cy >= 24) vpp_cy = 0;
					}
					if ((vpp_r & 0x10) && (old_cx != vpp_cx || old_cy != vpp_cy)) {
						dirty_row0 = old_cy;
						dirty_row1 = vpp_cy;
					}
				}
			}
			break;
		case 2:
			vpp_data = dat;
			break;
		case 3:
			switch (dat & 0xe0) {
				case 0x00:	/* plus_cmd_brow */
					if ((vpp_r & 0x10) && (vpp_cx != 0 || vpp_cy != (vpp_data & 0x1f))) {
						dirty_row0 = vpp_cy;
						dirty_row1 = vpp_data & 0x1f;
					}
					vpp_cy = vpp_data & 0x1f;
					vpp_cx = 0;
					break;
				case 0x20:	/* plus_cmd_loady */
					if ((vpp_r & 0x10) && (vpp_cy != (vpp_data & 0x1f))) {
						dirty_row0 = vpp_cy;
						dirty_row1 = vpp_data & 0x1f;
					}
					vpp_cy = vpp_data & 0x1f;
					break;
				case 0x40:	/* plus_cmd_loadx */
					if ((vpp_r & 0x10) && (vpp_cx != (vpp_data & 0x3f) % 40))
						dirty_row0 = vpp_cy;
					vpp_cx = (vpp_data & 0x3f) % 40;
					break;
				case 0x60:	/* plus_cmd_incc */
				{
					int old_cx = vpp_cx;
					int old_cy = vpp_cy;
					vpp_cx++;
					if (vpp_cx >= 40) {
						vpp_cx = 0;
						vpp_cy++;
						if (vpp_cy >= 24) vpp_cy = 0;
					}
					if ((vpp_r & 0x10) && (old_cx != vpp_cx || old_cy != vpp_cy)) {
						dirty_row0 = old_cy;
						dirty_row1 = vpp_cy;
					}
					break;
				}
				case 0x80:	/* plus_cmd_loadm */
					slicemode = 0;
					slice = (vpp_data & 0x1f) % 10;
					switch (vpp_data & 0xe0) {
						case 0x00:	/* plus_loadm_wr */
							inc_curs = 1;
							break;
						case 0x20:	/* plus_loadm_rd */
							inc_curs = 1;
							break;
						case 0x40:	/* plus_loadm_wrni */
							inc_curs = 0;
							break;
						case 0x60:	/* plus_loadm_rdni */
							inc_curs = 0;
							break;
						case 0x80:	/* plus_loadm_wrsl */
							slicemode = 1;
							break;
						case 0xA0:	/* plus_loadm_rdsl */
							slicemode = 1;
							break;
						default:
							break;
					}
					break;
				case 0xA0:	/* plus_cmd_loadr */
					if (vpp_r != vpp_data)
						dirty = 1;
					vpp_r = vpp_data;
					break;
				case 0xC0:	/* plus_cmd_loady0 */
				{
					int new_y0 = (vpp_data & 0x1f) % 24;
					if (vpp_y0 != new_y0)
						dirty = 1;
					vpp_y0 = new_y0;
					break;
				}
				default:
					break;
			}
			break;
		default:
			break;
	}

	if (dirty) {
		vpp_mark_dirty();
	} else {
#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
		if (dirty_row0 >= 0)
			vpp_mark_source_row_dirty(dirty_row0);
		if (dirty_row1 >= 0 && dirty_row1 != dirty_row0)
			vpp_mark_source_row_dirty(dirty_row1);
#else
		if (dirty_row0 >= 0 || dirty_row1 >= 0)
			vpp_mark_dirty();
#endif
	}
}

static void vpp_draw_char(int x, int y, uint8_t ch,
      uint8_t c0, uint8_t c1, uint8_t ext, uint8_t dw, uint8_t dh, uint8_t ul)
{
	int xx, yy, d, m, k;

	if ((x>39) || (y>24) || (ext>1)) return;

	d = (dh==2) ? 5 : 0;

	for (yy=0; yy<10; yy++) {
		if (ul && (d==9))
			k = 255;
		else if (ch >= 0xA0)
			k = dchars[ext][(ch-0xA0)*10 + d];
		else if (ch >= 0x80)
			k = 255;
		else
			k = vpp_cset[ext][ch * 10 + d];

		m = (dw==2) ? 0x08 : 0x80;

		for (xx=0; xx<8; xx++)
      {
			vppbmp->line[(y*10+yy)*vppbmp->w+(x*8+xx)] = (k & m) ? c1 : c0;
			if ((xx%2) || (dw==0)) m >>= 1;
		}
		if ((yy%2) || (dh==0)) d++;
	}

}

static void vpp_update_screen(void)
{
	int i,x,y,chr,attr,ext,dw,dh,vpar,lvd, ser_chr,ser_atr, swapcol;
	int tlum[8], m[8] = {0x01, 0x10, 0x04, 0x40, 0x02, 0x20, 0x08, 0x80};

	for (i=0; i<8; i++)
      tlum[i] = (LumReg & m[i]) ? 0 : 8;

	vpar = lvd = 0;
	for (y=0; y<25; y++)
   {
      int l;
      int c0   = 0;
      int lhd  = 0;
      int hpar = 0;
      int   ul = 0;
      int conc = 0;
      int box  = 0;
		vpar = (lvd==0) ? 0 : 1-vpar;

		l = (y==0) ? 31 : (y-1+vpp_y0)%24;

		for (x=0; x<40; x++)
      {
         int c1;

			hpar = (lhd==0) ? 0 : 1-hpar;

			chr = vpp_mem[x][l][0];
			attr = vpp_mem[x][l][1];
			c1 = attr & 0x7;
			c1 = ((c1&2) | ((c1&1)<<2) | ((c1&4)>>2));
			ext = (attr & 0x80) ? 1 : 0;

			ser_chr = vpp_mem[x][l][2];
			ser_atr = vpp_mem[x][l][3];
			if (ser_chr) {
				c0 = (ser_atr>>4) & 0x7;
				c0 = ((c0&2) | ((c0&1)<<2) | ((c0&4)>>2));
				ul = ser_chr & 4;
				conc = ser_chr & 1;
				box = ser_chr & 2;
			}

			if (ext) {
				c0 = (attr>>4) & 0x7;
				c0 = ((c0&2) | ((c0&1)<<2) | ((c0&4)>>2));
				dw = dh = 0;
			} else {
				dw = (attr & 0x20) ? (hpar ? 2 : 1) : 0;
				dh = (attr & 0x10) ? (vpar ? 2 : 1) : 0;
				if (dw) lhd=1;
				if (dh) lvd=1;
			}

			swapcol = 0;

			/* cursor display */
			if ((x == vpp_cx) && (l == vpp_cy)) {
				/* on cursor position */
				if (vpp_r & 0x10) {
					/* cursor display active */
					swapcol = !swapcol;
					if ((vpp_r & 0x80) && blink_st) {
						/* blinking active */
						swapcol = !swapcol;
					}
				}
			}

			/* invert attribute */
			if ((!ext) && (attr & 0x40)) swapcol = !swapcol;

			/* blinking chars */
			if ((vpp_r & 0x80) && !(attr & 8) && !blink_st) {
				/* cursor handling is done already */
				if (!(vpp_r & 0x10) || (x != vpp_cx) || (l != vpp_cy)) {
					c1=c0;
				}
			}

			if (((y == 0) && (vpp_r & 8)) || ((y != 0) && (vpp_r & 1))) {
				if ((!conc) || (!(vpp_r & 4))) {
					if (box || (!(vpp_r & 2))) {
						if (swapcol)
							vpp_draw_char(x, y, chr, c1|tlum[c1], c0|tlum[c0], ext, dw, dh, ul);
						else
							vpp_draw_char(x, y, chr, c0|tlum[c0], c1|tlum[c1], ext, dw, dh, ul);
					} else {
						vpp_draw_char(x, y, 255, (app_data.openb) ? 16 : 0, 0, 0, 0, 0, 0);
					}
				}
			}
		}
		
	}

	if (vpp_r & 0x20)
   {
		for (y = vppbmp->h-1; y >= 10; y--)
			for (x = 0; x < vppbmp->w; x++) 
				vppbmp->line[y*vppbmp->w +x] = vppbmp->line[((y-10)/2+10)*vppbmp->w +x];				;
	}

	need_update=0;
}

#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
static uint8_t vpp_char_bits(uint8_t ch, uint8_t ext, int row, uint8_t ul)
{
	if (row < 0)
		row = 0;
	if (row > 9)
		row = 9;
	if (ul && row == 9)
		return 0xff;
	if (ch >= 0xA0)
		return dchars[ext & 1][(ch - 0xA0) * 10 + row];
	if (ch >= 0x80)
		return 0xff;
	return vpp_cset[ext & 1][ch * 10 + row];
}

static uint8_t vpp_char_pixel_full(uint8_t ch,
                                   uint8_t ext,
                                   uint8_t dw,
                                   uint8_t dh,
                                   uint8_t ul,
                                   int px,
                                   int py)
{
	int row = (dh == 2) ? 5 : 0;
	uint8_t bits;
	uint8_t mask;

	if (dh == 0)
		row = py;
	else
		row += py / 2;
	if (row > 9)
		row = 9;

	bits = vpp_char_bits(ch, ext, row, ul);
	if (dw == 0)
		mask = (uint8_t)(0x80 >> px);
	else if (dw == 2)
		mask = (uint8_t)(0x08 >> (px / 2));
	else
		mask = (uint8_t)(0x80 >> (px / 2));

	return (bits & mask) ? 1 : 0;
}

typedef struct {
	uint8_t chr;
	uint8_t ext;
	uint8_t dw;
	uint8_t dh;
	uint8_t ul;
	uint8_t c0;
	uint8_t c1;
	uint8_t swapcol;
	uint8_t blank;
	uint8_t visible;
} VppCellState;

static VppCellState vpp_external_rows[25][40];
static int vpp_row_index_for_source_y(int sy);
static int vpp_row_pixel_for_source_y(int sy);

static void vpp_render_external_source_line(uint8_t out_line[320], int sy)
{
	int cell_x;
	int row;
	int py;
	const VppCellState *row_cells;

	if (!out_line)
		return;

	if (sy < 0 || sy >= 250) {
		memset(out_line, 0, 320);
		return;
	}

	row = vpp_row_index_for_source_y(sy);
	py = vpp_row_pixel_for_source_y(sy);
	row_cells = vpp_external_rows[row];

	for (cell_x = 0; cell_x < 40; ++cell_x) {
		const VppCellState *cell = &row_cells[cell_x];
		uint8_t *dst = out_line + (cell_x * 8);
		uint8_t fg;
		uint8_t bg;
		uint8_t bits;
		int row_idx;
		int px;

		if (!cell->visible || cell->blank) {
			memset(dst, 0, 8);
			continue;
		}

		if (cell->dh == 0)
			row_idx = py;
		else
			row_idx = ((cell->dh == 2) ? 5 : 0) + (py / 2);
		if (row_idx > 9)
			row_idx = 9;

		bits = vpp_char_bits(cell->chr, cell->ext, row_idx, cell->ul);
		fg = cell->swapcol ? cell->c0 : cell->c1;
		bg = cell->swapcol ? cell->c1 : cell->c0;

		switch (cell->dw) {
			case 2:
				for (px = 0; px < 8; ++px) {
					dst[px] = (bits & (uint8_t)(0x08 >> (px / 2))) ? fg : bg;
				}
				break;
			case 1:
				for (px = 0; px < 8; ++px) {
					dst[px] = (bits & (uint8_t)(0x80 >> (px / 2))) ? fg : bg;
				}
				break;
			default:
				for (px = 0; px < 8; ++px) {
					dst[px] = (bits & (uint8_t)(0x80 >> px)) ? fg : bg;
				}
				break;
		}
	}
}

static int vpp_row_index_for_source_y(int sy)
{
	if (vpp_r & 0x20) {
		if (sy >= 10)
			sy = ((sy - 10) / 2) + 10;
	}
	if (sy < 0)
		sy = 0;
	if (sy > 249)
		sy = 249;
	return sy / 10;
}

static int vpp_row_pixel_for_source_y(int sy)
{
	if (vpp_r & 0x20) {
		if (sy >= 10)
			sy = ((sy - 10) / 2) + 10;
	}
	if (sy < 0)
		sy = 0;
	if (sy > 249)
		sy = 249;
	return sy % 10;
}

static void vpp_build_vpar_table(int vpar_for_row[25])
{
	int row;
	int x;
	int vpar = 0;
	int lvd = 0;

	for (row = 0; row < 25; ++row) {
		int l = (row == 0) ? 31 : (row - 1 + vpp_y0) % 24;
		vpar = (lvd == 0) ? 0 : 1 - vpar;
		vpar_for_row[row] = vpar;
		for (x = 0; x < 40; ++x) {
			int attr = vpp_mem[x][l][1];
			int ext = (attr & 0x80) ? 1 : 0;
			if (!ext && (attr & 0x10))
				lvd = 1;
		}
	}
}

static void vpp_build_cell_row(int row, int vpar, const int tlum[8], VppCellState cells[40])
{
	int x;
	int l = (row == 0) ? 31 : (row - 1 + vpp_y0) % 24;
	int c0 = 0;
	int lhd = 0;
	int hpar = 0;
	int ul = 0;
	int conc = 0;
	int box = 0;

	for (x = 0; x < 40; ++x) {
		int c1;
		int chr, attr, ext, dw, dh, ser_chr, ser_atr, swapcol;

		hpar = (lhd == 0) ? 0 : 1 - hpar;

		chr = vpp_mem[x][l][0];
		attr = vpp_mem[x][l][1];
		c1 = attr & 0x7;
		c1 = ((c1 & 2) | ((c1 & 1) << 2) | ((c1 & 4) >> 2));
		ext = (attr & 0x80) ? 1 : 0;

		ser_chr = vpp_mem[x][l][2];
		ser_atr = vpp_mem[x][l][3];
		if (ser_chr) {
			c0 = (ser_atr >> 4) & 0x7;
			c0 = ((c0 & 2) | ((c0 & 1) << 2) | ((c0 & 4) >> 2));
			ul = ser_chr & 4;
			conc = ser_chr & 1;
			box = ser_chr & 2;
		}

		if (ext) {
			c0 = (attr >> 4) & 0x7;
			c0 = ((c0 & 2) | ((c0 & 1) << 2) | ((c0 & 4) >> 2));
			dw = dh = 0;
		} else {
			dw = (attr & 0x20) ? (hpar ? 2 : 1) : 0;
			dh = (attr & 0x10) ? (vpar ? 2 : 1) : 0;
			if (dw)
				lhd = 1;
		}

		swapcol = 0;
		if ((x == vpp_cx) && (l == vpp_cy) && (vpp_r & 0x10)) {
			swapcol = !swapcol;
			if ((vpp_r & 0x80) && blink_st)
				swapcol = !swapcol;
		}
		if ((!ext) && (attr & 0x40))
			swapcol = !swapcol;
		if ((vpp_r & 0x80) && !(attr & 8) && !blink_st) {
			if (!(vpp_r & 0x10) || (x != vpp_cx) || (l != vpp_cy))
				c1 = c0;
		}

		cells[x].chr = (uint8_t)chr;
		cells[x].ext = (uint8_t)ext;
		cells[x].dw = (uint8_t)dw;
		cells[x].dh = (uint8_t)dh;
		cells[x].ul = (uint8_t)ul;
		cells[x].c0 = (uint8_t)(c0 | tlum[c0]);
		cells[x].c1 = (uint8_t)(c1 | tlum[c1]);
		cells[x].swapcol = (uint8_t)swapcol;
		cells[x].blank = (uint8_t)(!box && (vpp_r & 2));
		cells[x].visible = (uint8_t)((((row == 0) && (vpp_r & 8)) || ((row != 0) && (vpp_r & 1))) &&
		                             ((!conc) || (!(vpp_r & 4))));
	}
}

static const VppCellState *vpp_get_cached_row(int row,
                                              const int vpar_for_row[25],
                                              const int tlum[8],
                                              VppCellState cache[2][40],
                                              int cache_rows[2],
                                              int *next_cache)
{
	int slot;

	if (cache_rows[0] == row)
		return cache[0];
	if (cache_rows[1] == row)
		return cache[1];

	slot = *next_cache;
	*next_cache = 1 - slot;
	vpp_build_cell_row(row, vpar_for_row[row], tlum, cache[slot]);
	cache_rows[slot] = row;
	return cache[slot];
}

static uint8_t vpp_sample_full_pixel_fast(const VppCellState cells[40],
                                          int sx,
                                          int py,
                                          uint8_t *foreground)
{
	const VppCellState *cell;
	uint8_t draw;
	int px;

	*foreground = 0;
	if (sx < 0 || sx >= 320 || py < 0 || py >= 10)
		return 0;

	cell = &cells[sx / 8];
	if (!cell->visible)
		return 0;

	px = sx & 7;
	if (cell->blank) {
		draw = vpp_char_pixel_full(255, 0, 0, 0, 0, px, py);
		*foreground = draw ? 1 : 0;
		return draw ? 0 : (uint8_t)((app_data.openb) ? 16 : 0);
	}

	draw = vpp_char_pixel_full(cell->chr, cell->ext, cell->dw, cell->dh, cell->ul, px, py);
	*foreground = draw ? 1 : 0;
	return draw ? (cell->swapcol ? cell->c0 : cell->c1)
	            : (cell->swapcol ? cell->c1 : cell->c0);
}

static uint8_t vpp_sample_low_pixel_fast(const VppCellState row0[40],
                                         int sx,
                                         int py0,
                                         const VppCellState row1[40],
                                         int py1,
                                         int has_row1)
{
	uint8_t fg;
	uint8_t first_color;
	uint8_t color;

	first_color = vpp_sample_full_pixel_fast(row0, sx, py0, &fg);
	if (fg)
		return first_color;
	color = vpp_sample_full_pixel_fast(row0, sx + 1, py0, &fg);
	if (fg)
		return color;
	if (has_row1) {
		color = vpp_sample_full_pixel_fast(row1, sx, py1, &fg);
		if (fg)
			return color;
		color = vpp_sample_full_pixel_fast(row1, sx + 1, py1, &fg);
		if (fg)
			return color;
	}

	return first_color;
}

static void vpp_render_low_memory_direct(uint8_t *vmem,
                                         int offx,
                                         int offy,
                                         int w,
                                         int h,
                                         int totw,
                                         int toth)
{
	int i, x, y;
	int tlum[8], m[8] = {0x01, 0x10, 0x04, 0x40, 0x02, 0x20, 0x08, 0x80};
	int tcol[16];
	int vpar_for_row[25];
	VppCellState row_cache[2][40];
	int row_cache_rows[2] = {-1, -1};
	int next_row_cache = 0;
	const int use_cache = (vpp_low_cache != NULL);
	int build_layer;

	if (!vmem)
		return;
	if (TraReg == 0xff) {
		vpp_low_cache_valid = 0;
		return;
	}

	frame_cnt--;
	if (frame_cnt <= 0) {
		frame_cnt = 100;
		blink_st = 1 - blink_st;
		vpp_mark_dirty();
	}

	for (i = 0; i < 8; i++) {
		tlum[i] = (LumReg & m[i]) ? 0 : 8;
		tcol[i] = tcol[i + 8] = !(TraReg & m[i]);
	}

	vpp_build_vpar_table(vpar_for_row);

	if (vpp_low_cache_openb != app_data.openb) {
		vpp_low_cache_openb = app_data.openb;
		vpp_mark_dirty();
	}

	build_layer = !use_cache || !vpp_low_cache_valid || need_update;
	if (use_cache && build_layer)
		memset(vpp_low_cache, 0, BMPW * BMPH);

	if (build_layer) {
		for (y = 0; y < h; ++y) {
			const int dy = offy + y;
			const int full_dest_y = dy * 2;
			int sy0 = full_dest_y - 5;
			int sy1 = sy0 + 1;
			int row0;
			int row1 = -1;
			int py0;
			int py1 = 0;
			int has_row1 = 0;
			const VppCellState *row_cells0;
			const VppCellState *row_cells1;
			int sx;

			if (dy < 0 || dy >= toth || sy0 < 0 || sy0 >= 250)
				continue;

			row0 = vpp_row_index_for_source_y(sy0);
			py0 = vpp_row_pixel_for_source_y(sy0);
			row_cells0 = vpp_get_cached_row(row0, vpar_for_row, tlum,
			                                row_cache, row_cache_rows,
			                                &next_row_cache);

			row_cells1 = row_cells0;
			if (sy1 >= 0 && sy1 < 250) {
				row1 = vpp_row_index_for_source_y(sy1);
				py1 = vpp_row_pixel_for_source_y(sy1);
				has_row1 = 1;
				if (row1 != row0) {
					row_cells1 = vpp_get_cached_row(row1, vpar_for_row, tlum,
					                                row_cache, row_cache_rows,
					                                &next_row_cache);
				}
			}

			sx = 11;
			for (x = 0; x < w; ++x) {
				const int dx = offx + x;
				uint8_t *dst;
				uint8_t c;
				uint8_t nc;

				if (dx < 0 || dx >= totw || sx < 0 || sx >= 320) {
					sx += 2;
					continue;
				}

				nc = vpp_sample_low_pixel_fast(row_cells0, sx, py0,
				                               row_cells1, py1, has_row1);
				if (use_cache) {
					vpp_low_cache[dy * totw + dx] = nc;
				} else {
					dst = vmem + dy * totw + dx;
					c = *dst;
					if (c < 16) {
						if (tcol[c & 0x0f]) {
							*dst = app_data.openb ? (uint8_t)(nc & 0x0f) : nc;
						} else if ((nc & 0x10) && app_data.openb) {
							*dst = (uint8_t)(nc & 0x0f);
						}
					}
				}
				sx += 2;
			}
		}
		if (use_cache)
			vpp_low_cache_valid = 1;
		need_update = 0;
	}

	if (use_cache) {
		for (y = 0; y < h; ++y) {
			const int dy = offy + y;

			if (dy < 0 || dy >= toth)
				continue;

			for (x = 0; x < w; ++x) {
				const int dx = offx + x;
				uint8_t *dst;
				uint8_t c;
				uint8_t nc;

				if (dx < 0 || dx >= totw)
					continue;

				dst = vmem + dy * totw + dx;
				c = *dst;
				if (c >= 16)
					continue;

				nc = vpp_low_cache[dy * totw + dx];
				if (tcol[c & 0x0f]) {
					*dst = app_data.openb ? (uint8_t)(nc & 0x0f) : nc;
				} else if ((nc & 0x10) && app_data.openb) {
					*dst = (uint8_t)(nc & 0x0f);
				}
			}
		}
	}
}

static int vpp_prepare_external_rows(void)
{
	int i;
	int tlum[8];
	int tcol[16];
	int m[8] = {0x01, 0x10, 0x04, 0x40, 0x02, 0x20, 0x08, 0x80};
	int vpar_for_row[25];
	int row;

	if (TraReg == 0xff) {
		vpp_external_cache_valid = 0;
		return 0;
	}

	if (vpp_external_openb != app_data.openb) {
		vpp_external_openb = app_data.openb;
		vpp_mark_dirty();
	}

	if (vpp_external_last_frame != frame) {
		vpp_external_last_frame = frame;
		frame_cnt--;
		if (frame_cnt <= 0) {
			frame_cnt = 100;
			blink_st = 1 - blink_st;
			vpp_mark_dirty();
		}
	}

	if (vpp_external_cache_valid && !need_update)
		return 1;

	for (i = 0; i < 8; ++i) {
		tlum[i] = (LumReg & m[i]) ? 0 : 8;
		tcol[i] = tcol[i + 8] = !(TraReg & m[i]);
	}
	(void)tcol;

	vpp_build_vpar_table(vpar_for_row);

	if (!vpp_external_cache_valid) {
		for (row = 0; row < 25; ++row) {
			vpp_build_cell_row(row, vpar_for_row[row], tlum, vpp_external_rows[row]);
			vpp_external_row_dirty[row] = 0;
		}
	} else {
		for (row = 0; row < 25; ++row) {
			if (!vpp_external_row_dirty[row])
				continue;
			vpp_build_cell_row(row, vpar_for_row[row], tlum, vpp_external_rows[row]);
			vpp_external_row_dirty[row] = 0;
		}
	}

	vpp_external_cache_valid = 1;
	need_update = 0;
	return 1;
}

void vpp_set_external_native_mode(int enabled)
{
	const int new_mode = enabled ? 1 : 0;
	if (vpp_external_native_mode == new_mode)
		return;
	vpp_external_native_mode = new_mode;
	vpp_external_line_sy = -9999;
	vpp_mark_dirty();
}

int vpp_render_external_line_rgb565(uint16_t *out_line,
                                    int dst_w,
                                    int dst_h,
                                    int dy,
                                    const uint8_t *base_line,
                                    int src_w,
                                    int src_h,
                                    const uint16_t palette[256])
{
	int dx;
	int sy;
	int tcol[16];
	int m[8] = {0x01, 0x10, 0x04, 0x40, 0x02, 0x20, 0x08, 0x80};
	int sx;

	if (!vpp_external_native_mode || !out_line || !base_line || !palette ||
	    dst_w <= 0 || dst_h <= 0 || dy < 0 || dy >= dst_h ||
	    src_w <= 0 || src_h <= 0 ||
	    dst_w > (int)(sizeof(vpp_external_base_xmap) / sizeof(vpp_external_base_xmap[0])) ||
	    dst_h > (int)(sizeof(vpp_external_sy_map) / sizeof(vpp_external_sy_map[0]))) {
		return 0;
	}

	if (!vpp_prepare_external_rows())
		return 0;

	if (vpp_external_map_dst_w != dst_w ||
	    vpp_external_map_dst_h != dst_h ||
	    vpp_external_map_src_w != src_w ||
	    vpp_external_map_src_h != src_h) {
		for (dx = 0; dx < dst_w; ++dx) {
			vpp_external_base_xmap[dx] = (uint16_t)((dx * src_w) / dst_w);
			vpp_external_overlay_xmap[dx] = (int16_t)(((dx * src_w * 2) / dst_w) - 5);
		}
		for (dx = 0; dx < dst_h; ++dx) {
			vpp_external_sy_map[dx] = (int16_t)(((dx * src_h * 2) / dst_h) - 5);
		}
		vpp_external_map_dst_w = dst_w;
		vpp_external_map_dst_h = dst_h;
		vpp_external_map_src_w = src_w;
		vpp_external_map_src_h = src_h;
		vpp_external_line_sy = -9999;
	}

	for (dx = 0; dx < 8; ++dx) {
		tcol[dx] = tcol[dx + 8] = !(TraReg & m[dx]);
	}

	sy = vpp_external_sy_map[dy];
	if (sy < 0 || sy >= 250) {
		for (dx = 0; dx < dst_w; ++dx) {
			const uint8_t base_idx = base_line[vpp_external_base_xmap[dx]];
			out_line[dx] = palette[base_idx];
		}
		return 1;
	}

	if (vpp_external_line_sy != sy) {
		vpp_render_external_source_line(vpp_external_line_cache, sy);
		vpp_external_line_sy = sy;
	}

	for (dx = 0; dx < dst_w; ++dx) {
		const uint8_t base_idx = base_line[vpp_external_base_xmap[dx]];
		uint8_t out_idx = base_idx;

		sx = vpp_external_overlay_xmap[dx];
		if (sx >= 0 && sx < 320 && base_idx < 16) {
			const uint8_t nc = vpp_external_line_cache[sx];

			if (tcol[base_idx & 0x0f]) {
				out_idx = app_data.openb ? (uint8_t)(nc & 0x0f) : nc;
			} else if ((nc & 0x10) && app_data.openb) {
				out_idx = (uint8_t)(nc & 0x0f);
			}
		}

		out_line[dx] = palette[out_idx];
	}

	return 1;
}

#if defined(VIDEOPAC_VPP_DUAL_CORE)
static void vpp_task_entry(void *arg)
{
	(void)arg;
	for (;;) {
		xSemaphoreTake(vpp_job_sem, portMAX_DELAY);
		if (vpp_job.stop)
			break;
		vpp_render_low_memory_direct(vpp_job.vmem, vpp_job.offx, vpp_job.offy,
		                             vpp_job.w, vpp_job.h, vpp_job.totw, vpp_job.toth);
		xSemaphoreGive(vpp_done_sem);
	}
	xSemaphoreGive(vpp_done_sem);
	vTaskDelete(NULL);
}

static int vpp_ensure_task(void)
{
	if (vpp_task_ready)
		return 1;
	if (!vpp_job_sem)
		vpp_job_sem = xSemaphoreCreateBinary();
	if (!vpp_done_sem)
		vpp_done_sem = xSemaphoreCreateBinary();
	if (!vpp_job_sem || !vpp_done_sem)
		return 0;
	memset(&vpp_job, 0, sizeof(vpp_job));
	if (xTaskCreatePinnedToCore(vpp_task_entry, "vpp+", 3072, NULL, 2,
	                            &vpp_task_handle, 0) != pdPASS)
		return 0;
	vpp_task_ready = 1;
	return 1;
}

static int vpp_render_low_memory(uint8_t *vmem, int offx, int offy, int w, int h, int totw, int toth)
{
	if (!vpp_ensure_task())
		return 0;

	while (xSemaphoreTake(vpp_done_sem, 0) == pdTRUE) {
	}

	vpp_job.vmem = vmem;
	vpp_job.offx = offx;
	vpp_job.offy = offy;
	vpp_job.w = w;
	vpp_job.h = h;
	vpp_job.totw = totw;
	vpp_job.toth = toth;
	vpp_job.stop = 0;
	xSemaphoreGive(vpp_job_sem);
	xSemaphoreTake(vpp_done_sem, portMAX_DELAY);
	return 1;
}

static void vpp_stop_task(void)
{
	if (!vpp_task_ready)
		return;
	vpp_job.stop = 1;
	xSemaphoreGive(vpp_job_sem);
	xSemaphoreTake(vpp_done_sem, pdMS_TO_TICKS(50));
	vpp_task_handle = NULL;
	vpp_task_ready = 0;
}
#else
static int vpp_render_low_memory(uint8_t *vmem, int offx, int offy, int w, int h, int totw, int toth)
{
	vpp_render_low_memory_direct(vmem, offx, offy, w, h, totw, toth);
	return 1;
}

static void vpp_stop_task(void)
{
}
#endif
#else
void vpp_set_external_native_mode(int enabled)
{
	(void)enabled;
}

int vpp_render_external_line_rgb565(uint16_t *out_line,
                                    int dst_w,
                                    int dst_h,
                                    int dy,
                                    const uint8_t *base_line,
                                    int src_w,
                                    int src_h,
                                    const uint16_t palette[256])
{
	(void)out_line;
	(void)dst_w;
	(void)dst_h;
	(void)dy;
	(void)base_line;
	(void)src_w;
	(void)src_h;
	(void)palette;
	return 0;
}
#endif

void vpp_finish_bmp(uint8_t *vmem, int offx, int offy, int w, int h, int totw, int toth){
#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
	if (vpp_external_native_mode)
		return;
	if (vpp_render_low_memory(vmem, offx, offy, w, h, totw, toth))
		return;
	vpp_render_low_memory_direct(vmem, offx, offy, w, h, totw, toth);
#else
	int i, x, y, t, c, nc, clrx, clry;
	int tcol[16], m[8] = {0x01, 0x10, 0x04, 0x40, 0x02, 0x20, 0x08, 0x80};
	uint8_t *pnt, *pnt2, *pnt3;

	if (!vppbmp || !colplus) return;

	if (vppon) {
		memset(colplus,0,BMPW*BMPH);
		vppon=0;
	}

	if (TraReg == 0xff) return;

	vppon=1;
	
	frame_cnt--;
	if (frame_cnt<=0) {
		frame_cnt = 100;
		blink_st = 1-blink_st;
		vpp_mark_dirty();
	}

	if (need_update)
      vpp_update_screen();

	for (i=0; i<8; i++) tcol[i] = tcol[i+8] = !(TraReg & m[i]);
		
	if (w > totw-offx) w = totw-offx;
	if (h > toth-offy) h = toth-offy;

	if (w > vppbmp->w) w = vppbmp->w;
	if (h > vppbmp->h) h = vppbmp->h;
	
	clrx = clry = 0;
	for (i=0; (!clrx) && (i<totw); i++) if (tcol[vmem[offy*totw+i]&7]) clrx=1;
	for (i=0; (!clry) && (i<toth); i++) if (tcol[vmem[i*totw+offx]&7]) clry=1;
	if (clrx) for (y=0; y<offy; y++) for (x=0; x<totw; x++) vmem[y*totw+x]=0;
	if (clry) for (y=0; y<toth; y++) for (x=0; x<offx; x++) vmem[y*totw+x]=0;

	for (y=0; y<h; y++){
		pnt = vmem+(offy+y)*totw + offx;
		pnt2 = (uint8_t *)&vppbmp->line[y*vppbmp->w];


		x=0;
		while (x < w) {
			pnt3 = pnt;
			c = *pnt++;
			t = x++;

			if ((((x+offx) & 3) == 0) && (sizeof(unsigned long)==4)) {
				unsigned long cccc, dddd, *p = (unsigned long*) pnt;
				int t2=x, w2=w-4;
				cccc = (((unsigned long)c) & 0xff) | ((((unsigned long)c) & 0xff) << 8) | ((((unsigned long)c) & 0xff) << 16) | ((((unsigned long)c) & 0xff) << 24);
				dddd = *p++;
				while ((x<w2) && (dddd == cccc)) {
					x += 4;
					dddd = *p++;
				}
				pnt += x-t2;
			}

			if (c<16) {
				if (tcol[c]){
					if (app_data.openb)
						for (i=0; i<x-t; i++) *pnt3++ = *pnt2++ & 0xf;
					else {

						memcpy(pnt3, pnt2, x-t);
						pnt2 += x-t;

					}
				} else {
					for (i=0; i<x-t; i++) {
						nc = *pnt2++;
						if ((nc & 0x10) && app_data.openb) {
							*pnt3++ = nc & 0xf;
						} else if (nc & 8) {
							colplus[pnt3++ - vmem] = 0x40;
						} else {
							pnt3++;
						}
					}

				}
			}
			
		}

	}

#endif
}

void load_colplus(uint8_t *col){
#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
	if (col)
		memset(col,0,BMPW*BMPH);
#else
	if (vppon && colplus)
		memcpy(col,colplus,BMPW*BMPH);
	else
		memset(col,0,BMPW*BMPH);
#endif
}


void init_vpp(void)
{
	int i,j,k;

#if !defined(VIDEOPAC_LOW_MEMORY_VIDEO)
	if (!vppbmp) vppbmp = create_bitmap(320,250);
	if (!colplus) colplus = (uint8_t *)malloc(BMPW*BMPH);

	if ((!vppbmp) || (!colplus))
		exit(EXIT_FAILURE);
	memset(colplus,0,BMPW*BMPH);
#else
	vppbmp = NULL;
	colplus = NULL;
	vpp_low_cache = g_emu_static_pool + (BMPW * BMPH * 2u);
	vpp_low_cache_valid = 0;
	vpp_low_cache_openb = -1;
	vpp_external_native_mode = 0;
	vpp_external_cache_valid = 0;
	vpp_external_last_frame = -1;
	vpp_external_openb = -1;
	vpp_external_line_sy = -9999;
	vpp_external_map_dst_w = -1;
	vpp_external_map_dst_h = -1;
	vpp_external_map_src_w = -1;
	vpp_external_map_src_h = -1;
	memset(vpp_external_row_dirty, 0, sizeof(vpp_external_row_dirty));
	memset(vpp_low_cache, 0, BMPW * BMPH);
	printf("[O2EM]: Low-memory Videopac+ VPP renderer ready (%ux%u%s).\n",
	       (unsigned)BMPW,
	       (unsigned)BMPH,
#if defined(VIDEOPAC_VPP_DUAL_CORE)
	       ", dual-core"
#else
	       ""
#endif
	);
#endif

	LumReg = TraReg = 0xff;
	vpp_cx = 0;
	vpp_cy = 0;
	vpp_y0 = 0;
	vpp_r = 0;
	inc_curs = 1;
	vpp_data = 0;
	frame_cnt=0;
	blink_st=0;
	slice = 0;
	slicemode=0;
	vpp_mark_dirty();
	vppon = 1;

	for (i=0; i<2; i++)
		for (j=0; j<960; j++) dchars[i][j] = 0;

	for (i=0; i<40; i++)
		for (j=0; j<32; j++)
			for (k=0; k<4; k++) vpp_mem[i][j][k] = 0;
}

void close_vpp(void)
{
#if defined(VIDEOPAC_LOW_MEMORY_VIDEO)
	vpp_stop_task();
	vpp_low_cache = NULL;
	vpp_low_cache_valid = 0;
	vpp_low_cache_openb = -1;
	vpp_external_native_mode = 0;
	vpp_external_cache_valid = 0;
	vpp_external_last_frame = -1;
	vpp_external_openb = -1;
	vpp_external_line_sy = -9999;
	vpp_external_map_dst_w = -1;
	vpp_external_map_dst_h = -1;
	vpp_external_map_src_w = -1;
	vpp_external_map_src_h = -1;
	memset(vpp_external_row_dirty, 0, sizeof(vpp_external_row_dirty));
	vppbmp = NULL;
	colplus = NULL;
#else
	destroy_bitmap(vppbmp);
	vppbmp = NULL;

	if (colplus)
		free(colplus);
	colplus = NULL;
#endif
}
