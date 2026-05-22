/* This file is part of Snes9x. See LICENSE file. */

#include "snes9x.h"

#include "memmap.h"
#include "ppu.h"
#include "display.h"
#include "gfx.h"
#include "tile.h"

static const uint32_t HeadMask[4] =
{
#ifdef MSB_FIRST
   0xffffffff, 0x00ffffff, 0x0000ffff, 0x000000ff
#else
   0xffffffff, 0xffffff00, 0xffff0000, 0xff000000
#endif
};

static const uint32_t TailMask[5] =
{
#ifdef MSB_FIRST
   0x00000000, 0xff000000, 0xffff0000, 0xffffff00, 0xffffffff
#else
   0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff, 0xffffffff
#endif
};

static const uint32_t odd[4][16] =
{
#ifdef MSB_FIRST
   {0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101, 0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101},
   {0x00000000, 0x00000004, 0x00000400, 0x00000404, 0x00040000, 0x00040004, 0x00040400, 0x00040404, 0x04000000, 0x04000004, 0x04000400, 0x04000404, 0x04040000, 0x04040004, 0x04040400, 0x04040404},
   {0x00000000, 0x00000010, 0x00001000, 0x00001010, 0x00100000, 0x00100010, 0x00101000, 0x00101010, 0x10000000, 0x10000010, 0x10001000, 0x10001010, 0x10100000, 0x10100010, 0x10101000, 0x10101010},
   {0x00000000, 0x00000040, 0x00004000, 0x00004040, 0x00400000, 0x00400040, 0x00404000, 0x00404040, 0x40000000, 0x40000040, 0x40004000, 0x40004040, 0x40400000, 0x40400040, 0x40404000, 0x40404040}
#else
   {0x00000000, 0x01000000, 0x00010000, 0x01010000, 0x00000100, 0x01000100, 0x00010100, 0x01010100, 0x00000001, 0x01000001, 0x00010001, 0x01010001, 0x00000101, 0x01000101, 0x00010101, 0x01010101},
   {0x00000000, 0x04000000, 0x00040000, 0x04040000, 0x00000400, 0x04000400, 0x00040400, 0x04040400, 0x00000004, 0x04000004, 0x00040004, 0x04040004, 0x00000404, 0x04000404, 0x00040404, 0x04040404},
   {0x00000000, 0x10000000, 0x00100000, 0x10100000, 0x00001000, 0x10001000, 0x00101000, 0x10101000, 0x00000010, 0x10000010, 0x00100010, 0x10100010, 0x00001010, 0x10001010, 0x00101010, 0x10101010},
   {0x00000000, 0x40000000, 0x00400000, 0x40400000, 0x00004000, 0x40004000, 0x00404000, 0x40404000, 0x00000040, 0x40000040, 0x00400040, 0x40400040, 0x00004040, 0x40004040, 0x00404040, 0x40404040}
#endif
};

static const uint32_t even[4][16] =
{
#ifdef MSB_FIRST
   {0x00000000, 0x00000002, 0x00000200, 0x00000202, 0x00020000, 0x00020002, 0x00020200, 0x00020202, 0x02000000, 0x02000002, 0x02000200, 0x02000202, 0x02020000, 0x02020002, 0x02020200, 0x02020202},
   {0x00000000, 0x00000008, 0x00000800, 0x00000808, 0x00080000, 0x00080008, 0x00080800, 0x00080808, 0x08000000, 0x08000008, 0x08000800, 0x08000808, 0x08080000, 0x08080008, 0x08080800, 0x08080808},
   {0x00000000, 0x00000020, 0x00002000, 0x00002020, 0x00200000, 0x00200020, 0x00202000, 0x00202020, 0x20000000, 0x20000020, 0x20002000, 0x20002020, 0x20200000, 0x20200020, 0x20202000, 0x20202020},
   {0x00000000, 0x00000080, 0x00008000, 0x00008080, 0x00800000, 0x00800080, 0x00808000, 0x00808080, 0x80000000, 0x80000080, 0x80008000, 0x80008080, 0x80800000, 0x80800080, 0x80808000, 0x80808080}
#else
   {0x00000000, 0x02000000, 0x00020000, 0x02020000, 0x00000200, 0x02000200, 0x00020200, 0x02020200, 0x00000002, 0x02000002, 0x00020002, 0x02020002, 0x00000202, 0x02000202, 0x00020202, 0x02020202},
   {0x00000000, 0x08000000, 0x00080000, 0x08080000, 0x00000800, 0x08000800, 0x00080800, 0x08080800, 0x00000008, 0x08000008, 0x00080008, 0x08080008, 0x00000808, 0x08000808, 0x00080808, 0x08080808},
   {0x00000000, 0x20000000, 0x00200000, 0x20200000, 0x00002000, 0x20002000, 0x00202000, 0x20202000, 0x00000020, 0x20000020, 0x00200020, 0x20200020, 0x00002020, 0x20002020, 0x00202020, 0x20202020},
   {0x00000000, 0x80000000, 0x00800000, 0x80800000, 0x00008000, 0x80008000, 0x00808000, 0x80808000, 0x00000080, 0x80000080, 0x00800080, 0x80800080, 0x00008080, 0x80008080, 0x00808080, 0x80808080}
#endif
};

static uint8_t ConvertTile(uint8_t* pCache, uint32_t TileAddr)
{
   uint8_t* tp = &Memory.VRAM[TileAddr];
   uint32_t* p = (uint32_t*) pCache;
   uint32_t non_zero = 0;
   uint8_t line;
   uint32_t p1;
   uint32_t p2;
   uint8_t pix;

   switch (BG.BitShift)
   {
   case 8:
      for (line = 8; line != 0; line--, tp += 2)
      {
         p1 = p2 = 0;
         if((pix = tp[0]))
         {
            p1 |= odd[0][pix >> 4];
            p2 |= odd[0][pix & 0xf];
         }
         if((pix = tp[1]))
         {
            p1 |= even[0][pix >> 4];
            p2 |= even[0][pix & 0xf];
         }
         if((pix = tp[16]))
         {
            p1 |= odd[1][pix >> 4];
            p2 |= odd[1][pix & 0xf];
         }
         if((pix = tp[17]))
         {
            p1 |= even[1][pix >> 4];
            p2 |= even[1][pix & 0xf];
         }
         if((pix = tp[32]))
         {
            p1 |= odd[2][pix >> 4];
            p2 |= odd[2][pix & 0xf];
         }
         if((pix = tp[33]))
         {
            p1 |= even[2][pix >> 4];
            p2 |= even[2][pix & 0xf];
         }
         if((pix = tp[48]))
         {
            p1 |= odd[3][pix >> 4];
            p2 |= odd[3][pix & 0xf];
         }
         if((pix = tp[49]))
         {
            p1 |= even[3][pix >> 4];
            p2 |= even[3][pix & 0xf];
         }
         *p++ = p1;
         *p++ = p2;
         non_zero |= p1 | p2;
      }
      break;
   case 4:
      for (line = 8; line != 0; line--, tp += 2)
      {
         p1 = p2 = 0;
#ifdef SNES_BRANCHLESS_CONVERT_TILE_4BPP
         pix = tp[0];
         p1 |= odd[0][pix >> 4];
         p2 |= odd[0][pix & 0xf];
         pix = tp[1];
         p1 |= even[0][pix >> 4];
         p2 |= even[0][pix & 0xf];
         pix = tp[16];
         p1 |= odd[1][pix >> 4];
         p2 |= odd[1][pix & 0xf];
         pix = tp[17];
         p1 |= even[1][pix >> 4];
         p2 |= even[1][pix & 0xf];
#else
         if((pix = tp[0]))
         {
            p1 |= odd[0][pix >> 4];
            p2 |= odd[0][pix & 0xf];
         }
         if((pix = tp[1]))
         {
            p1 |= even[0][pix >> 4];
            p2 |= even[0][pix & 0xf];
         }
         if((pix = tp[16]))
         {
            p1 |= odd[1][pix >> 4];
            p2 |= odd[1][pix & 0xf];
         }
         if((pix = tp[17]))
         {
            p1 |= even[1][pix >> 4];
            p2 |= even[1][pix & 0xf];
         }
#endif
         *p++ = p1;
         *p++ = p2;
         non_zero |= p1 | p2;
      }
      break;
   case 2:
      for (line = 8; line != 0; line--, tp += 2)
      {
         p1 = p2 = 0;
         if((pix = tp[0]))
         {
            p1 |= odd[0][pix >> 4];
            p2 |= odd[0][pix & 0xf];
         }
         if((pix = tp[1]))
         {
            p1 |= even[0][pix >> 4];
            p2 |= even[0][pix & 0xf];
         }
         *p++ = p1;
         *p++ = p2;
         non_zero |= p1 | p2;
      }
      break;
   }
   return non_zero ? (0x10|BG.Depth) : BLANK_TILE;
}

typedef struct
{
   uint32_t key;
   uint32_t epoch;
   uint8_t  state;
   uint8_t  pixels[64];
} SSmallTileCacheEntry;

uint32_t S9xSmallTileCacheEpoch = 1;

static SSmallTileCacheEntry* s_smallTileCache = NULL;
static uint32_t              s_smallTileCacheMask = 0;
static uint32_t              s_smallTileCacheEntries = 0;

static uint32_t S9xRoundDownPow2(uint32_t value)
{
   uint32_t result = 1;
   while ((result << 1) != 0 && (result << 1) <= value)
      result <<= 1;
   return result;
}

bool S9xInitSmallTileCache(uint32_t entries)
{
   if (entries < 32)
      entries = 32;
   entries = S9xRoundDownPow2(entries);

   S9xDeinitSmallTileCache();

   s_smallTileCache = (SSmallTileCacheEntry*) calloc(entries, sizeof(*s_smallTileCache));
   if (!s_smallTileCache)
      return false;

   s_smallTileCacheEntries = entries;
   s_smallTileCacheMask = entries - 1;
   S9xSmallTileCacheEpoch++;
   return true;
}

void S9xDeinitSmallTileCache(void)
{
   free(s_smallTileCache);
   s_smallTileCache = NULL;
   s_smallTileCacheMask = 0;
   s_smallTileCacheEntries = 0;
   S9xSmallTileCacheEpoch++;
}

bool S9xSmallTileCacheEnabled(void)
{
   return s_smallTileCache != NULL;
}

uint32_t S9xSmallTileCacheEntries(void)
{
   return s_smallTileCacheEntries;
}

uint32_t S9xSmallTileCacheBytes(void)
{
   return s_smallTileCacheEntries * (uint32_t) sizeof(*s_smallTileCache);
}

static INLINE uint8_t* S9xSmallTileCacheFetch(uint32_t TileAddr, uint8_t* conv)
{
   if (!s_smallTileCache)
      return NULL;

   const uint32_t epoch = S9xSmallTileCacheEpoch;
   const uint32_t key = TileAddr ^ (BG.BitShift << 16) ^ ((uint32_t) BG.Depth << 24);
   const uint32_t slot = ((TileAddr >> 4) ^ (TileAddr >> 9) ^ (BG.BitShift * 17u)) & s_smallTileCacheMask;
   SSmallTileCacheEntry* entry = &s_smallTileCache[slot];

   if (entry->state && entry->key == key && entry->epoch == epoch)
   {
      *conv = entry->state;
      return entry->pixels;
   }

   *conv = ConvertTile(entry->pixels, TileAddr);
   entry->key = key;
   entry->epoch = epoch;
   entry->state = *conv;
   return entry->pixels;
}

#define PLOT_PIXEL(screen, pixel) (pixel)

#define SNES_STRINGIFY_VALUE(x) #x
#define SNES_STRINGIFY(x) SNES_STRINGIFY_VALUE(x)

#if (defined(__XTENSA__) || defined(__xtensa__)) && defined(__GNUC__) && !defined(NO_ASM)
#define WRITE_PIXEL16_XTENSA(Screen, Depth, PixelValue, ScreenColors, DepthOffset, ScreenByteOffset) \
   do                                                                                               \
   {                                                                                                \
      uint8_t Pixel = (uint8_t) (PixelValue);                                                       \
      if (GFX.Z1 > (Depth)[DepthOffset] && Pixel)                                                   \
      {                                                                                             \
         uint32_t Color;                                                                            \
         __asm__ __volatile__ (                                                                     \
            "slli  %[Color], %[Pixel], 1        \n"                                                 \
            "add   %[Color], %[Pal], %[Color]   \n"                                                 \
            "l16ui %[Color], %[Color], 0        \n"                                                 \
            "s8i   %[ZSet], %[Z], " SNES_STRINGIFY(DepthOffset) "\n"                               \
            "s16i  %[Color], %[Out], " SNES_STRINGIFY(ScreenByteOffset) "\n"                       \
            : [Color] "=&r" (Color)                                                                \
            : [Out] "r" (Screen), [Z] "r" (Depth), [Pixel] "r" ((uint32_t) Pixel),                 \
              [Pal] "r" (ScreenColors), [ZSet] "r" ((uint32_t) GFX.Z2)                             \
            : "memory"                                                                             \
         );                                                                                         \
      }                                                                                             \
   } while (0)

#define WRITE_PIXEL16_X2_XTENSA(Screen, Depth, PixelValue, ScreenColors, DepthOffset, ScreenByteOffset) \
   do                                                                                                  \
   {                                                                                                   \
      uint8_t Pixel = (uint8_t) (PixelValue);                                                          \
      if (GFX.Z1 > (Depth)[DepthOffset] && Pixel)                                                      \
      {                                                                                                \
         uint32_t Color;                                                                               \
         __asm__ __volatile__ (                                                                        \
            "slli  %[Color], %[Pixel], 1        \n"                                                    \
            "add   %[Color], %[Pal], %[Color]   \n"                                                    \
            "l16ui %[Color], %[Color], 0        \n"                                                    \
            "s8i   %[ZSet], %[Z], " SNES_STRINGIFY(DepthOffset) "\n"                                  \
            "s8i   %[ZSet], %[Z], " SNES_STRINGIFY(DepthOffset + 1) "\n"                              \
            "s16i  %[Color], %[Out], " SNES_STRINGIFY(ScreenByteOffset) "\n"                          \
            "s16i  %[Color], %[Out], " SNES_STRINGIFY(ScreenByteOffset + 2) "\n"                      \
            : [Color] "=&r" (Color)                                                                   \
            : [Out] "r" (Screen), [Z] "r" (Depth), [Pixel] "r" ((uint32_t) Pixel),                    \
              [Pal] "r" (ScreenColors), [ZSet] "r" ((uint32_t) GFX.Z2)                                \
            : "memory"                                                                                \
         );                                                                                            \
      }                                                                                                \
   } while (0)

#define WRITE_PIXEL16_HALF_XTENSA(Screen, Depth, PixelValue, ScreenColors, DepthCompareOffset, DepthStoreOffset, ScreenByteOffset) \
   do                                                                                                                           \
   {                                                                                                                            \
      uint8_t Pixel = (uint8_t) (PixelValue);                                                                                   \
      if (GFX.Z1 > (Depth)[DepthCompareOffset] && Pixel)                                                                        \
      {                                                                                                                         \
         uint32_t Color;                                                                                                        \
         __asm__ __volatile__ (                                                                                                 \
            "slli  %[Color], %[Pixel], 1        \n"                                                                             \
            "add   %[Color], %[Pal], %[Color]   \n"                                                                             \
            "l16ui %[Color], %[Color], 0        \n"                                                                             \
            "s8i   %[ZSet], %[Z], " SNES_STRINGIFY(DepthStoreOffset) "\n"                                                      \
            "s16i  %[Color], %[Out], " SNES_STRINGIFY(ScreenByteOffset) "\n"                                                   \
            : [Color] "=&r" (Color)                                                                                            \
            : [Out] "r" (Screen), [Z] "r" (Depth), [Pixel] "r" ((uint32_t) Pixel),                                             \
              [Pal] "r" (ScreenColors), [ZSet] "r" ((uint32_t) GFX.Z2)                                                         \
            : "memory"                                                                                                         \
         );                                                                                                                     \
      }                                                                                                                         \
   } while (0)
#endif

static INLINE void WRITE_4PIXELS16(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
#if (defined(__XTENSA__) || defined(__xtensa__)) && defined(__GNUC__) && !defined(NO_ASM)
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth  = GFX.DB + Offset;
   WRITE_PIXEL16_XTENSA(Screen, Depth, Pixels[0], ScreenColors, 0, 0);
   WRITE_PIXEL16_XTENSA(Screen, Depth, Pixels[1], ScreenColors, 1, 2);
   WRITE_PIXEL16_XTENSA(Screen, Depth, Pixels[2], ScreenColors, 2, 4);
   WRITE_PIXEL16_XTENSA(Screen, Depth, Pixels[3], ScreenColors, 3, 6);
#else
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         Screen [N] = ScreenColors [Pixel];
         Depth [N] = GFX.Z2;
      }
   }
#endif
}

static INLINE void WRITE_4PIXELS16_FLIPPED(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
#if (defined(__XTENSA__) || defined(__xtensa__)) && defined(__GNUC__) && !defined(NO_ASM)
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth  = GFX.DB + Offset;
   WRITE_PIXEL16_XTENSA(Screen, Depth, Pixels[3], ScreenColors, 0, 0);
   WRITE_PIXEL16_XTENSA(Screen, Depth, Pixels[2], ScreenColors, 1, 2);
   WRITE_PIXEL16_XTENSA(Screen, Depth, Pixels[1], ScreenColors, 2, 4);
   WRITE_PIXEL16_XTENSA(Screen, Depth, Pixels[0], ScreenColors, 3, 6);
#else
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N]))
      {
         Screen [N] = ScreenColors [Pixel];
         Depth [N] = GFX.Z2;
      }
   }
#endif
}

static void WRITE_4PIXELS16_HALFWIDTH(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
#if (defined(__XTENSA__) || defined(__xtensa__)) && defined(__GNUC__) && !defined(NO_ASM)
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth  = GFX.DB + Offset;
   WRITE_PIXEL16_HALF_XTENSA(Screen, Depth, Pixels[0], ScreenColors, 0, 0, 0);
   WRITE_PIXEL16_HALF_XTENSA(Screen, Depth, Pixels[2], ScreenColors, 2, 1, 2);
#else
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N += 2)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         Screen [N >> 1] = ScreenColors [Pixel];
         Depth [N >> 1] = GFX.Z2;
      }
   }
#endif
}

static void WRITE_4PIXELS16_FLIPPED_HALFWIDTH(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
#if (defined(__XTENSA__) || defined(__xtensa__)) && defined(__GNUC__) && !defined(NO_ASM)
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth  = GFX.DB + Offset;
   WRITE_PIXEL16_HALF_XTENSA(Screen, Depth, Pixels[2], ScreenColors, 0, 0, 0);
   WRITE_PIXEL16_HALF_XTENSA(Screen, Depth, Pixels[0], ScreenColors, 2, 1, 2);
#else
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N += 2)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[2 - N]))
      {
         Screen [N >> 1] = ScreenColors [Pixel];
         Depth [N >> 1] = GFX.Z2;
      }
   }
#endif
}

static void WRITE_4PIXELS16x2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
#if (defined(__XTENSA__) || defined(__xtensa__)) && defined(__GNUC__) && !defined(NO_ASM)
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth  = GFX.DB + Offset;
   WRITE_PIXEL16_X2_XTENSA(Screen, Depth, Pixels[0], ScreenColors, 0, 0);
   WRITE_PIXEL16_X2_XTENSA(Screen, Depth, Pixels[1], ScreenColors, 2, 4);
   WRITE_PIXEL16_X2_XTENSA(Screen, Depth, Pixels[2], ScreenColors, 4, 8);
   WRITE_PIXEL16_X2_XTENSA(Screen, Depth, Pixels[3], ScreenColors, 6, 12);
#else
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[N]))
      {
         Screen [N * 2] = Screen [N * 2 + 1] = ScreenColors [Pixel];
         Depth [N * 2] = Depth [N * 2 + 1] = GFX.Z2;
      }
   }
#endif
}

static void WRITE_4PIXELS16_FLIPPEDx2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
#if (defined(__XTENSA__) || defined(__xtensa__)) && defined(__GNUC__) && !defined(NO_ASM)
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth  = GFX.DB + Offset;
   WRITE_PIXEL16_X2_XTENSA(Screen, Depth, Pixels[3], ScreenColors, 0, 0);
   WRITE_PIXEL16_X2_XTENSA(Screen, Depth, Pixels[2], ScreenColors, 2, 4);
   WRITE_PIXEL16_X2_XTENSA(Screen, Depth, Pixels[1], ScreenColors, 4, 8);
   WRITE_PIXEL16_X2_XTENSA(Screen, Depth, Pixels[0], ScreenColors, 6, 12);
#else
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[3 - N]))
      {
         Screen [N * 2] = Screen [N * 2 + 1] = ScreenColors [Pixel];
         Depth [N * 2] = Depth [N * 2 + 1] = GFX.Z2;
      }
   }
#endif
}

static void WRITE_4PIXELS16x2x2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[N]))
      {
         Screen [N * 2] = Screen [N * 2 + 1] = Screen [(GFX.RealPitch >> 1) + N * 2] = Screen [(GFX.RealPitch >> 1) + N * 2 + 1] = ScreenColors [Pixel];
         Depth [N * 2] = Depth [N * 2 + 1] = Depth [(GFX.RealPitch >> 1) + N * 2] = Depth [(GFX.RealPitch >> 1) + N * 2 + 1] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPEDx2x2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.DB + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N * 2] && (Pixel = Pixels[3 - N]))
      {
         Screen [N * 2] = Screen [N * 2 + 1] = Screen [(GFX.RealPitch >> 1) + N * 2] = Screen [(GFX.RealPitch >> 1) + N * 2 + 1] = ScreenColors [Pixel];
         Depth [N * 2] = Depth [N * 2 + 1] = Depth [(GFX.RealPitch >> 1) + N * 2] = Depth [(GFX.RealPitch >> 1) + N * 2 + 1] = GFX.Z2;
      }
   }
}

void DrawTile16(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16, WRITE_4PIXELS16_FLIPPED, 4);
}

void DrawClippedTile16(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16, WRITE_4PIXELS16_FLIPPED, 4);
}

void DrawTile16HalfWidth(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16_HALFWIDTH, WRITE_4PIXELS16_FLIPPED_HALFWIDTH, 2);
}

void DrawClippedTile16HalfWidth(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16_HALFWIDTH, WRITE_4PIXELS16_FLIPPED_HALFWIDTH, 2);
}

void DrawTile16x2(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16x2, WRITE_4PIXELS16_FLIPPEDx2, 8);
}

void DrawClippedTile16x2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16x2, WRITE_4PIXELS16_FLIPPEDx2, 8);
}

void DrawTile16x2x2(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16x2x2, WRITE_4PIXELS16_FLIPPEDx2x2, 8);
}

void DrawClippedTile16x2x2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16x2x2, WRITE_4PIXELS16_FLIPPEDx2x2, 8);
}

void DrawLargePixel16(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t pixel;
   uint16_t *sp;
   uint8_t *Depth;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   sp = (uint16_t*) GFX.S + Offset;
   Depth = GFX.DB + Offset;
   RENDER_TILE_LARGE(ScreenColors [pixel], PLOT_PIXEL);
}

void DrawLargePixel16HalfWidth(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t pixel;
   uint16_t *sp;
   uint8_t *Depth;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   sp = (uint16_t*) GFX.S + Offset;
   Depth = GFX.DB + Offset;
   RENDER_TILE_LARGE_HALFWIDTH(ScreenColors [pixel], PLOT_PIXEL);
}

static void WRITE_4PIXELS16_ADD(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         switch (SubDepth [N])
         {
         case 0:
            Screen [N] = ScreenColors [Pixel];
            break;
         case 1:
            Screen [N] = COLOR_ADD(ScreenColors [Pixel], GFX.FixedColour);
            break;
         default:
            Screen [N] = COLOR_ADD(ScreenColors [Pixel], Screen [GFX.Delta + N]);
            break;
         }
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPED_ADD(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N]))
      {
         switch (SubDepth [N])
         {
         case 0:
            Screen [N] = ScreenColors [Pixel];
            break;
         case 1:
            Screen [N] = COLOR_ADD(ScreenColors [Pixel], GFX.FixedColour);
            break;
         default:
            Screen [N] = COLOR_ADD(ScreenColors [Pixel], Screen [GFX.Delta + N]);
            break;
         }
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_ADD1_2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         switch (SubDepth [N])
         {
         case 0:
            Screen [N] = ScreenColors [Pixel];
            break;
         case 1:
            Screen [N] = COLOR_ADD(ScreenColors [Pixel], GFX.FixedColour);
            break;
         default:
            Screen [N] = (uint16_t)(COLOR_ADD1_2(ScreenColors [Pixel], Screen [GFX.Delta + N]));
            break;
         }
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPED_ADD1_2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N]))
      {
         switch (SubDepth [N])
         {
         case 0:
            Screen [N] = ScreenColors [Pixel];
            break;
         case 1:
            Screen [N] = COLOR_ADD(ScreenColors [Pixel], GFX.FixedColour);
            break;
         default:
            Screen [N] = (uint16_t)(COLOR_ADD1_2(ScreenColors [Pixel], Screen [GFX.Delta + N]));
            break;
         }
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_SUB(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         switch (SubDepth [N])
         {
         case 0:
            Screen [N] = ScreenColors [Pixel];
            break;
         case 1:
            Screen [N] = (uint16_t) COLOR_SUB(ScreenColors [Pixel], GFX.FixedColour);
            break;
         default:
            Screen [N] = (uint16_t) COLOR_SUB(ScreenColors [Pixel], Screen [GFX.Delta + N]);
            break;
         }
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPED_SUB(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N]))
      {
         switch (SubDepth [N])
         {
         case 0:
            Screen [N] = ScreenColors [Pixel];
            break;
         case 1:
            Screen [N] = (uint16_t) COLOR_SUB(ScreenColors [Pixel], GFX.FixedColour);
            break;
         default:
            Screen [N] = (uint16_t) COLOR_SUB(ScreenColors [Pixel], Screen [GFX.Delta + N]);
            break;
         }
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_SUB1_2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         switch (SubDepth [N])
         {
         case 0:
            Screen [N] = ScreenColors [Pixel];
            break;
         case 1:
            Screen [N] = (uint16_t) COLOR_SUB(ScreenColors [Pixel], GFX.FixedColour);
            break;
         default:
            Screen [N] = (uint16_t) COLOR_SUB1_2(ScreenColors [Pixel], Screen [GFX.Delta + N]);
            break;
         }
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPED_SUB1_2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N]))
      {
         switch (SubDepth [N])
         {
         case 0:
            Screen [N] = ScreenColors [Pixel];
            break;
         case 1:
            Screen [N] = (uint16_t) COLOR_SUB(ScreenColors [Pixel], GFX.FixedColour);
            break;
         default:
            Screen [N] = (uint16_t) COLOR_SUB1_2(ScreenColors [Pixel], Screen [GFX.Delta + N]);
            break;
         }
         Depth [N] = GFX.Z2;
      }
   }
}


void DrawTile16Add(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   uint8_t Pixel;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t* Depth = GFX.ZBuffer + Offset;
   uint8_t* SubDepth = GFX.SubZBuffer + Offset;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();

   switch (Tile & (V_FLIP | H_FLIP))
   {
   case 0:
      bp = pCache + StartLine;
      for (l = LineCount; l != 0; l--, bp += 8, Screen += GFX.PPL, Depth += GFX.PPL, SubDepth += GFX.PPL)
      {
         uint8_t N;
         for (N = 0; N < 8; N++)
         {
            if (GFX.Z1 > Depth [N] && (Pixel = bp[N]))
            {
               switch (SubDepth [N])
               {
               case 0:
                  Screen [N] = ScreenColors [Pixel];
                  break;
               case 1:
                  Screen [N] = COLOR_ADD(ScreenColors [Pixel], GFX.FixedColour);
                  break;
               default:
                  Screen [N] = COLOR_ADD(ScreenColors [Pixel], Screen [GFX.Delta + N]);
                  break;
               }
               Depth [N] = GFX.Z2;
            }
         }
      }
      break;
   case H_FLIP:
      bp = pCache + StartLine;
      for (l = LineCount; l != 0; l--, bp += 8, Screen += GFX.PPL, Depth += GFX.PPL, SubDepth += GFX.PPL)
      {
         uint8_t N;
         for (N = 0; N < 8; N++)
         {
            if (GFX.Z1 > Depth [N] && (Pixel = bp[7 - N]))
            {
               switch (SubDepth [N])
               {
               case 0:
                  Screen [N] = ScreenColors [Pixel];
                  break;
               case 1:
                  Screen [N] = COLOR_ADD(ScreenColors [Pixel], GFX.FixedColour);
                  break;
               default:
                  Screen [N] = COLOR_ADD(ScreenColors [Pixel], Screen [GFX.Delta + N]);
                  break;
               }
               Depth [N] = GFX.Z2;
            }
         }
      }
      break;
   case H_FLIP | V_FLIP:
      bp = pCache + 56 - StartLine;
      for (l = LineCount; l != 0; l--, bp -= 8, Screen += GFX.PPL, Depth += GFX.PPL, SubDepth += GFX.PPL)
      {
         uint8_t N;
         for (N = 0; N < 8; N++)
         {
            if (GFX.Z1 > Depth [N] && (Pixel = bp[7 - N]))
            {
               switch (SubDepth [N])
               {
               case 0:
                  Screen [N] = ScreenColors [Pixel];
                  break;
               case 1:
                  Screen [N] = COLOR_ADD(ScreenColors [Pixel], GFX.FixedColour);
                  break;
               default:
                  Screen [N] = COLOR_ADD(ScreenColors [Pixel], Screen [GFX.Delta + N]);
                  break;
               }
               Depth [N] = GFX.Z2;
            }
         }
      }
      break;
   case V_FLIP:
      bp = pCache + 56 - StartLine;
      for (l = LineCount; l != 0; l--, bp -= 8, Screen += GFX.PPL, Depth += GFX.PPL, SubDepth += GFX.PPL)
      {
         uint8_t N;
         for (N = 0; N < 8; N++)
         {
            if (GFX.Z1 > Depth [N] && (Pixel = bp[N]))
            {
               switch (SubDepth [N])
               {
               case 0:
                  Screen [N] = ScreenColors [Pixel];
                  break;
               case 1:
                  Screen [N] = COLOR_ADD(ScreenColors [Pixel], GFX.FixedColour);
                  break;
               default:
                  Screen [N] = COLOR_ADD(ScreenColors [Pixel], Screen [GFX.Delta + N]);
                  break;
               }
               Depth [N] = GFX.Z2;
            }
         }
      }
      break;
   default:
      break;
   }
}

void DrawClippedTile16Add(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16_ADD, WRITE_4PIXELS16_FLIPPED_ADD, 4);
}

void DrawTile16Add1_2(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16_ADD1_2, WRITE_4PIXELS16_FLIPPED_ADD1_2, 4);
}

void DrawClippedTile16Add1_2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16_ADD1_2, WRITE_4PIXELS16_FLIPPED_ADD1_2, 4);
}

void DrawTile16Sub(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16_SUB, WRITE_4PIXELS16_FLIPPED_SUB, 4);
}

void DrawClippedTile16Sub(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width,  uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16_SUB, WRITE_4PIXELS16_FLIPPED_SUB, 4);
}

void DrawTile16Sub1_2(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16_SUB1_2, WRITE_4PIXELS16_FLIPPED_SUB1_2, 4);
}

void DrawClippedTile16Sub1_2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16_SUB1_2, WRITE_4PIXELS16_FLIPPED_SUB1_2, 4);
}

static void WRITE_4PIXELS16_ADDF1_2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         if (SubDepth [N] == 1)
            Screen [N] = (uint16_t)(COLOR_ADD1_2(ScreenColors [Pixel], GFX.FixedColour));
         else
            Screen [N] = ScreenColors [Pixel];
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPED_ADDF1_2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N]))
      {
         if (SubDepth [N] == 1)
            Screen [N] = (uint16_t)(COLOR_ADD1_2(ScreenColors [Pixel], GFX.FixedColour));
         else
            Screen [N] = ScreenColors [Pixel];
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_SUBF1_2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[N]))
      {
         if (SubDepth [N] == 1)
            Screen [N] = (uint16_t) COLOR_SUB1_2(ScreenColors [Pixel], GFX.FixedColour);
         else
            Screen [N] = ScreenColors [Pixel];
         Depth [N] = GFX.Z2;
      }
   }
}

static void WRITE_4PIXELS16_FLIPPED_SUBF1_2(int32_t Offset, uint8_t* Pixels, uint16_t* ScreenColors)
{
   uint8_t  Pixel, N;
   uint16_t* Screen = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint8_t*  SubDepth = GFX.SubZBuffer + Offset;

   for (N = 0; N < 4; N++)
   {
      if (GFX.Z1 > Depth [N] && (Pixel = Pixels[3 - N]))
      {
         if (SubDepth [N] == 1)
            Screen [N] = (uint16_t) COLOR_SUB1_2(ScreenColors [Pixel], GFX.FixedColour);
         else
            Screen [N] = ScreenColors [Pixel];
         Depth [N] = GFX.Z2;
      }
   }
}

void DrawTile16FixedAdd1_2(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16_ADDF1_2, WRITE_4PIXELS16_FLIPPED_ADDF1_2, 4);
}

void DrawClippedTile16FixedAdd1_2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width,  uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16_ADDF1_2, WRITE_4PIXELS16_FLIPPED_ADDF1_2, 4);
}

void DrawTile16FixedSub1_2(uint32_t Tile, int32_t Offset, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();
   RENDER_TILE(WRITE_4PIXELS16_SUBF1_2, WRITE_4PIXELS16_FLIPPED_SUBF1_2, 4);
}

void DrawClippedTile16FixedSub1_2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Width, uint32_t StartLine, uint32_t LineCount)
{
   uint8_t* bp;
   TILE_PREAMBLE_VARS();
   TILE_CLIP_PREAMBLE_VARS();
   RENDER_CLIPPED_TILE_VARS();
   TILE_PREAMBLE_CODE();
   TILE_CLIP_PREAMBLE_CODE();
   RENDER_CLIPPED_TILE_CODE(WRITE_4PIXELS16_SUBF1_2, WRITE_4PIXELS16_FLIPPED_SUBF1_2, 4);
}

void DrawLargePixel16Add(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t* sp = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint16_t pixel;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();

#define LARGE_ADD_PIXEL(s, p) \
(Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_ADD (p, *(s + GFX.Delta)) : \
                COLOR_ADD (p, GFX.FixedColour)) : p)

   RENDER_TILE_LARGE(ScreenColors [pixel], LARGE_ADD_PIXEL);
}

void DrawLargePixel16Add1_2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t* sp = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint16_t pixel;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();

#define LARGE_ADD_PIXEL1_2(s, p) \
((uint16_t) (Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_ADD1_2 (p, *(s + GFX.Delta)) : \
                COLOR_ADD (p, GFX.FixedColour)) : p))

   RENDER_TILE_LARGE(ScreenColors [pixel], LARGE_ADD_PIXEL1_2);
}

void DrawLargePixel16Sub(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t* sp = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint16_t pixel;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();

#define LARGE_SUB_PIXEL(s, p) \
(Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_SUB (p, *(s + GFX.Delta)) : \
                COLOR_SUB (p, GFX.FixedColour)) : p)

   RENDER_TILE_LARGE(ScreenColors [pixel], LARGE_SUB_PIXEL);
}

void DrawLargePixel16Sub1_2(uint32_t Tile, int32_t Offset, uint32_t StartPixel, uint32_t Pixels, uint32_t StartLine, uint32_t LineCount)
{
   uint16_t* sp = (uint16_t*) GFX.S + Offset;
   uint8_t*  Depth = GFX.ZBuffer + Offset;
   uint16_t pixel;
   TILE_PREAMBLE_VARS();
   TILE_PREAMBLE_CODE();

#define LARGE_SUB_PIXEL1_2(s, p) \
(Depth [z + GFX.DepthDelta] ? (Depth [z + GFX.DepthDelta] != 1 ? \
                COLOR_SUB1_2 (p, *(s + GFX.Delta)) : \
                COLOR_SUB (p, GFX.FixedColour)) : p)

   RENDER_TILE_LARGE(ScreenColors [pixel], LARGE_SUB_PIXEL1_2);
}
