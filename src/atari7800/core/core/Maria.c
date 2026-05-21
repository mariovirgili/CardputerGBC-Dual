/* ----------------------------------------------------------------------------
 *   ___  ___  ___  ___       ___  ____  ___  _  _
 *  /__/ /__/ /  / /__  /__/ /__    /   /_   / |/ /
 * /    / \  /__/ ___/ ___/ ___/   /   /__  /    /  emulator
 *
 * ----------------------------------------------------------------------------
 * Copyright 2005 Greg Stanton
 * ----------------------------------------------------------------------------
 * Maria.c - optimized rewrite (no IRAM)
 * ----------------------------------------------------------------------------
 */
#include "Maria.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "Equates.h"
#include "Pair.h"
#include "Memory.h"
#include "Sally.h"
#include "Cartridge.h"
#include "Region.h"

#define MARIA_LINERAM_SIZE 160

#if defined(__GNUC__)
#define MARIA_LIKELY(x)   __builtin_expect(!!(x), 1)
#define MARIA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define MARIA_LIKELY(x)   (x)
#define MARIA_UNLIKELY(x) (x)
#endif

#define MARIA_INLINE static inline

rect maria_displayArea = {0, 16, 319, 258};
rect maria_visibleArea = {0, 26, 319, 248};

uint8_t* maria_surface = NULL;
uint32_t maria_surface_size = MARIA_SURFACE_SIZE;
uint16_t maria_scanline = 1;

bool maria_skip_render = false;
uint8_t* maria_lineRAM = NULL;

static uint32_t maria_cycles;
static pair maria_dpp;
static pair maria_dp;
static pair maria_pp;
static uint8_t maria_horizontal;
static uint8_t maria_palette;
static int8_t maria_offset;
static uint8_t maria_h08;
static uint8_t maria_h16;
static uint8_t maria_wmode;

/* CTRL bits cached once per DLL walk */
static uint8_t s_maria_kmode = 0;
static uint8_t s_maria_cwidth = 0;

/* runtime LUTs only */
static uint8_t* s_rmode2_lut_a = NULL;
static uint8_t* s_rmode2_lut_b = NULL;
static uint8_t* s_rmode3_lut_a = NULL;
static uint8_t* s_rmode3_lut_b = NULL;

static void* maria_Alloc(size_t size)
{
#ifdef ESP_PLATFORM
   void* p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
   if(p)
      return p;
#endif
   return malloc(size);
}

static bool maria_EnsureLUTs(void)
{
   int i;

   if(s_rmode2_lut_a && s_rmode2_lut_b && s_rmode3_lut_a && s_rmode3_lut_b)
      return true;

   if(!s_rmode2_lut_a)
      s_rmode2_lut_a = (uint8_t*)maria_Alloc(256);
   if(!s_rmode2_lut_b)
      s_rmode2_lut_b = (uint8_t*)maria_Alloc(256);
   if(!s_rmode3_lut_a)
      s_rmode3_lut_a = (uint8_t*)maria_Alloc(256);
   if(!s_rmode3_lut_b)
      s_rmode3_lut_b = (uint8_t*)maria_Alloc(256);

   if(!(s_rmode2_lut_a && s_rmode2_lut_b && s_rmode3_lut_a && s_rmode3_lut_b))
      return false;

   for(i = 0; i < 256; ++i)
   {
      s_rmode2_lut_a[i] = (uint8_t)((i & 16) | ((i & 8) >> 3) | (i & 2));
      s_rmode2_lut_b[i] = (uint8_t)((i & 16) | ((i & 4) >> 2) | ((i & 1) << 1));
      s_rmode3_lut_a[i] = (uint8_t)(i & 30);
      s_rmode3_lut_b[i] = (uint8_t)((i & 28) | ((i & 1) << 1));
   }

   return true;
}

bool maria_EnsureAllocated(void)
{
   if(!maria_surface)
   {
      maria_surface_size = (cartridge_region == REGION_PAL)
                           ? MARIA_SURFACE_SIZE_PAL
                           : MARIA_SURFACE_SIZE_NTSC;

      maria_surface = (uint8_t*)maria_Alloc(maria_surface_size);
      if(!maria_surface)
      {
#ifdef A7800_LOGS
         EMU_LOG("[A7800][MARIA] surface allocation FAILED (%u bytes)\n",
                 (unsigned)maria_surface_size);
#endif
      }
   }

   if(!maria_lineRAM)
   {
      maria_lineRAM = (uint8_t*)maria_Alloc(MARIA_LINERAM_SIZE);
      if(!maria_lineRAM)
      {
#ifdef A7800_LOGS
         EMU_LOG("[A7800][MARIA] lineRAM allocation FAILED (%u bytes)\n",
                 (unsigned)MARIA_LINERAM_SIZE);
#endif
      }
   }

   if(!(maria_surface && maria_lineRAM))
      return false;

   /* optional perf boost only */
   maria_EnsureLUTs();

   return true;
}

bool maria_IsReady(void)
{
   return maria_surface != NULL && maria_lineRAM != NULL;
}

void maria_Shutdown(void)
{
   if(maria_surface)
   {
      free(maria_surface);
      maria_surface = NULL;
   }

   if(maria_lineRAM)
   {
      free(maria_lineRAM);
      maria_lineRAM = NULL;
   }

   if(s_rmode2_lut_a)
   {
      free(s_rmode2_lut_a);
      s_rmode2_lut_a = NULL;
   }

   if(s_rmode2_lut_b)
   {
      free(s_rmode2_lut_b);
      s_rmode2_lut_b = NULL;
   }

   if(s_rmode3_lut_a)
   {
      free(s_rmode3_lut_a);
      s_rmode3_lut_a = NULL;
   }

   if(s_rmode3_lut_b)
   {
      free(s_rmode3_lut_b);
      s_rmode3_lut_b = NULL;
   }
}

MARIA_INLINE uint8_t maria_ReadByte(uint16_t address)
{
   uint32_t page, chrOffset;

   if(cartridge_type != CARTRIDGE_TYPE_SOUPER)
      return mem_rd(address);

   if((cartridge_souper_mode & CARTRIDGE_SOUPER_MODE_MFT) == 0 ||
      address < 0x8000 ||
      ((cartridge_souper_mode & CARTRIDGE_SOUPER_MODE_CHR) == 0 && address < 0xc000))
   {
      return memory_Read(address);
   }

   if(address >= 0xc000)
      return memory_Read((uint16_t)(address - 0x8000));

   if(address < 0xa000)
      return memory_Read((uint16_t)(address + 0x4000));

   page      = (uint16_t)cartridge_souper_chr_bank[(address & 0x80) ? 1 : 0];
   chrOffset = (((page & 0xfe) << 4) | (page & 1)) << 7;
   return cartridge_LoadROM((address & 0x0f7f) | chrOffset);
}

MARIA_INLINE void maria_StoreCell2(uint8_t data)
{
   uint8_t h = maria_horizontal;

   if(MARIA_LIKELY(!maria_skip_render) && h < MARIA_LINERAM_SIZE)
   {
      if(data)
         maria_lineRAM[h] = (uint8_t)(maria_palette | data);
      else if(s_maria_kmode)
         maria_lineRAM[h] = 0;
   }

   maria_horizontal = (uint8_t)(h + 1);
}

MARIA_INLINE void maria_StoreCell(uint8_t high, uint8_t low)
{
   uint8_t h = maria_horizontal;

   if(MARIA_LIKELY(!maria_skip_render) && h < MARIA_LINERAM_SIZE)
   {
      if(low || high)
         maria_lineRAM[h] = (uint8_t)((maria_palette & 16) | high | low);
      else if(s_maria_kmode)
         maria_lineRAM[h] = 0;
   }

   maria_horizontal = (uint8_t)(h + 1);
}

MARIA_INLINE uint8_t maria_IsHolyDMA_Fast(uint16_t addr)
{
   if(addr > 32767)
   {
      if(maria_h16 && (addr & 4096))
         return 1;
      if(maria_h08 && (addr & 2048))
         return 1;
   }
   return 0;
}

MARIA_INLINE void maria_StoreGraphic(void)
{
   uint16_t addr = maria_pp.w;
   uint8_t data = maria_ReadByte(addr);
   uint8_t holy = maria_IsHolyDMA_Fast(addr);

   maria_pp.w = (uint16_t)(addr + 1);

   if(maria_wmode)
   {
      if(holy)
      {
         maria_StoreCell(0, 0);
         maria_StoreCell(0, 0);
      }
      else
      {
         maria_StoreCell((uint8_t)(data & 12), (uint8_t)((data & 192) >> 6));
         maria_StoreCell((uint8_t)((data & 48) >> 4), (uint8_t)((data & 3) << 2));
      }
   }
   else
   {
      if(holy)
      {
         maria_StoreCell2(0);
         maria_StoreCell2(0);
         maria_StoreCell2(0);
         maria_StoreCell2(0);
      }
      else
      {
         maria_StoreCell2((uint8_t)((data & 192) >> 6));
         maria_StoreCell2((uint8_t)((data & 48) >> 4));
         maria_StoreCell2((uint8_t)((data & 12) >> 2));
         maria_StoreCell2((uint8_t)(data & 3));
      }
   }
}

static void maria_WriteLineRAM(uint8_t* buffer)
{
   uint8_t bg[32];
   uint8_t rmode;
   uint8_t* dst = buffer;
   uint8_t* src = maria_lineRAM;
   int i;

   for(i = 0; i < 32; ++i)
      bg[i] = maria_ReadByte((uint16_t)(BACKGRND + i));

   rmode = (uint8_t)(maria_ReadByte(CTRL) & 3);

   if(rmode == 0)
   {
      for(i = 0; i < MARIA_LINERAM_SIZE; i += 4)
      {
         uint8_t s0 = src[i + 0];
         uint8_t s1 = src[i + 1];
         uint8_t s2 = src[i + 2];
         uint8_t s3 = src[i + 3];

         uint8_t c0 = bg[s0 & 31];
         uint8_t c1 = bg[s1 & 31];
         uint8_t c2 = bg[s2 & 31];
         uint8_t c3 = bg[s3 & 31];

         dst[0] = c0; dst[1] = c0;
         dst[2] = c1; dst[3] = c1;
         dst[4] = c2; dst[5] = c2;
         dst[6] = c3; dst[7] = c3;
         dst += 8;
      }
   }
   else if(rmode == 2)
   {
      if(s_rmode2_lut_a && s_rmode2_lut_b)
      {
         uint8_t* la = s_rmode2_lut_a;
         uint8_t* lb = s_rmode2_lut_b;

         for(i = 0; i < MARIA_LINERAM_SIZE; i += 4)
         {
            uint8_t s0 = src[i + 0];
            uint8_t s1 = src[i + 1];
            uint8_t s2 = src[i + 2];
            uint8_t s3 = src[i + 3];

            dst[0] = bg[la[s0]];
            dst[1] = bg[lb[s0]];
            dst[2] = bg[la[s1]];
            dst[3] = bg[lb[s1]];
            dst[4] = bg[la[s2]];
            dst[5] = bg[lb[s2]];
            dst[6] = bg[la[s3]];
            dst[7] = bg[lb[s3]];
            dst += 8;
         }
      }
      else
      {
         for(i = 0; i < MARIA_LINERAM_SIZE; i += 4)
         {
            uint8_t s0 = src[i + 0];
            uint8_t s1 = src[i + 1];
            uint8_t s2 = src[i + 2];
            uint8_t s3 = src[i + 3];

            dst[0] = bg[(s0 & 16) | ((s0 & 8) >> 3) | (s0 & 2)];
            dst[1] = bg[(s0 & 16) | ((s0 & 4) >> 2) | ((s0 & 1) << 1)];
            dst[2] = bg[(s1 & 16) | ((s1 & 8) >> 3) | (s1 & 2)];
            dst[3] = bg[(s1 & 16) | ((s1 & 4) >> 2) | ((s1 & 1) << 1)];
            dst[4] = bg[(s2 & 16) | ((s2 & 8) >> 3) | (s2 & 2)];
            dst[5] = bg[(s2 & 16) | ((s2 & 4) >> 2) | ((s2 & 1) << 1)];
            dst[6] = bg[(s3 & 16) | ((s3 & 8) >> 3) | (s3 & 2)];
            dst[7] = bg[(s3 & 16) | ((s3 & 4) >> 2) | ((s3 & 1) << 1)];
            dst += 8;
         }
      }
   }
   else if(rmode == 3)
   {
      if(s_rmode3_lut_a && s_rmode3_lut_b)
      {
         uint8_t* la = s_rmode3_lut_a;
         uint8_t* lb = s_rmode3_lut_b;

         for(i = 0; i < MARIA_LINERAM_SIZE; i += 4)
         {
            uint8_t s0 = src[i + 0];
            uint8_t s1 = src[i + 1];
            uint8_t s2 = src[i + 2];
            uint8_t s3 = src[i + 3];

            dst[0] = bg[la[s0]];
            dst[1] = bg[lb[s0]];
            dst[2] = bg[la[s1]];
            dst[3] = bg[lb[s1]];
            dst[4] = bg[la[s2]];
            dst[5] = bg[lb[s2]];
            dst[6] = bg[la[s3]];
            dst[7] = bg[lb[s3]];
            dst += 8;
         }
      }
      else
      {
         for(i = 0; i < MARIA_LINERAM_SIZE; i += 4)
         {
            uint8_t s0 = src[i + 0];
            uint8_t s1 = src[i + 1];
            uint8_t s2 = src[i + 2];
            uint8_t s3 = src[i + 3];

            dst[0] = bg[s0 & 30];
            dst[1] = bg[(s0 & 28) | ((s0 & 1) << 1)];
            dst[2] = bg[s1 & 30];
            dst[3] = bg[(s1 & 28) | ((s1 & 1) << 1)];
            dst[4] = bg[s2 & 30];
            dst[5] = bg[(s2 & 28) | ((s2 & 1) << 1)];
            dst[6] = bg[s3 & 30];
            dst[7] = bg[(s3 & 28) | ((s3 & 1) << 1)];
            dst += 8;
         }
      }
   }
}

static void maria_StoreLineRAM(void)
{
   uint16_t dpw;
   uint8_t mode;

   {
      uint8_t ctrl = mem_rd((uint16_t)CTRL);
      s_maria_kmode = (uint8_t)(ctrl & 4);
      s_maria_cwidth = (uint8_t)(ctrl & 16);
   }

   if(MARIA_LIKELY(!maria_skip_render))
      memset(maria_lineRAM, 0, MARIA_LINERAM_SIZE);

   dpw = maria_dp.w;
   mode = maria_ReadByte((uint16_t)(dpw + 1));

   while(mode & 0x5f)
   {
      uint8_t width;
      uint8_t indirect = 0;
      uint8_t b0 = maria_ReadByte(dpw);
      uint8_t b1 = maria_ReadByte((uint16_t)(dpw + 1));
      uint8_t b2 = maria_ReadByte((uint16_t)(dpw + 2));

      maria_pp.b.l = b0;
      maria_pp.b.h = b2;

      if(b1 & 31)
      {
         maria_cycles += 8;
         maria_palette = (uint8_t)((b1 & 224) >> 3);
         maria_horizontal = maria_ReadByte((uint16_t)(dpw + 3));
         width = (uint8_t)(((~(b1 & 31)) & 31) + 1);
         maria_wmode = 0;
         dpw = (uint16_t)(dpw + 4);
      }
      else
      {
         uint8_t b3 = maria_ReadByte((uint16_t)(dpw + 3));
         uint8_t raww = (uint8_t)(b3 & 31);

         maria_cycles += 10;
         maria_palette = (uint8_t)((b3 & 224) >> 3);
         maria_horizontal = maria_ReadByte((uint16_t)(dpw + 4));
         indirect = (uint8_t)(b1 & 32);
         maria_wmode = (uint8_t)(b1 & 128);
         width = (raww == 0) ? 32 : (uint8_t)(((~raww) & 31) + 1);
         dpw = (uint16_t)(dpw + 5);
      }

      if(!indirect)
      {
         uint8_t hi = (uint8_t)(maria_pp.b.h + maria_offset);
         maria_pp.b.h = hi;

         for(uint8_t i = 0; i < width; ++i)
         {
            maria_cycles += 3;
            maria_StoreGraphic();
         }
      }
      else
      {
         uint16_t basePP = maria_pp.w;
         uint8_t hi = (uint8_t)(maria_ReadByte(CHARBASE) + maria_offset);

         for(uint8_t i = 0; i < width; ++i)
         {
            maria_cycles += 3;
            maria_pp.b.l = maria_ReadByte(basePP++);
            maria_pp.b.h = hi;

            maria_cycles += 6;
            maria_StoreGraphic();

            if(s_maria_cwidth)
            {
               maria_cycles += 3;
               maria_StoreGraphic();
            }
         }
      }

      mode = maria_ReadByte((uint16_t)(dpw + 1));
   }

   maria_dp.w = dpw;
}

void maria_Reset(void)
{
   maria_scanline = 1;

   if(!maria_EnsureAllocated())
      return;

   memset(maria_surface, 0, maria_surface_size);
}

uint32_t maria_RenderScanline(void)
{
   uint8_t ctrl;

   if(!maria_IsReady())
      return 0;

   maria_cycles = 0;
   ctrl = maria_ReadByte(CTRL);

   if((ctrl & 96) == 64 &&
      maria_scanline >= maria_displayArea.top &&
      maria_scanline <= maria_displayArea.bottom)
   {
      maria_cycles += 31;

      if(maria_scanline == maria_displayArea.top)
      {
         uint16_t dppw;
         uint8_t d0, d1, d2;

         maria_cycles += 7;
         maria_dpp.b.l = maria_ReadByte(DPPL);
         maria_dpp.b.h = maria_ReadByte(DPPH);

         dppw = maria_dpp.w;
         d0 = maria_ReadByte(dppw);
         d1 = maria_ReadByte((uint16_t)(dppw + 1));
         d2 = maria_ReadByte((uint16_t)(dppw + 2));

         maria_h08 = (uint8_t)(d0 & 32);
         maria_h16 = (uint8_t)(d0 & 64);
         maria_offset = (int8_t)(d0 & 15);
         maria_dp.b.h = d1;
         maria_dp.b.l = d2;

         if(d0 & 128)
            sally_ExecuteNMI();
      }
      else if(!maria_skip_render &&
              maria_scanline >= maria_visibleArea.top &&
              maria_scanline <= maria_visibleArea.bottom)
      {
         uint16_t stride = (uint16_t)Rect_GetLength(&maria_displayArea);
         maria_WriteLineRAM(maria_surface + ((maria_scanline - maria_displayArea.top) * stride));
      }

      if(maria_scanline != maria_displayArea.bottom)
      {
         uint16_t dppw = maria_dpp.w;
         maria_dp.b.l = maria_ReadByte((uint16_t)(dppw + 2));
         maria_dp.b.h = maria_ReadByte((uint16_t)(dppw + 1));
         maria_StoreLineRAM();
         maria_offset--;

         if(maria_offset < 0)
         {
            uint8_t d0;
            maria_dpp.w = (uint16_t)(maria_dpp.w + 3);
            d0 = maria_ReadByte(maria_dpp.w);
            maria_h08 = (uint8_t)(d0 & 32);
            maria_h16 = (uint8_t)(d0 & 64);
            maria_offset = (int8_t)(d0 & 15);

            if(d0 & 128)
               sally_ExecuteNMI();
         }
      }
   }

   return maria_cycles;
}

void maria_Clear(void)
{
   if(!maria_IsReady())
      return;

   memset(maria_surface, 0, maria_surface_size);
}
