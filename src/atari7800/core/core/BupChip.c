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
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * ----------------------------------------------------------------------------
 * BupChip.c
 * ----------------------------------------------------------------------------
 */
#include "BupChip.h"
#include "Cartridge.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "libretro.h"

#define BUPCHIP_FLAGS_PLAYING  1
#define BUPCHIP_FLAGS_PAUSED   2

struct BupchipFileContents
{
   uint8_t *data;
   size_t size;
};

typedef struct BupchipFileContents BupchipFileContents;

uint8_t *bupchip_sample_data;
uint8_t *bupchip_instrument_data;
BupchipFileContents *bupchip_songs = NULL;
uint8_t bupchip_song_count;

uint8_t bupchip_flags;
uint8_t bupchip_volume;
uint8_t bupchip_current_song;

short *bupchip_buffer = NULL;

static void* bupchip_Alloc(size_t size)
{
#ifdef ESP_PLATFORM
   void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if(!ptr)
      ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
   return ptr;
#else
   return malloc(size);
#endif
}

static bool bupchip_EnsureAllocated(void)
{
   if(!bupchip_buffer)
   {
      const size_t bufferSize = sizeof(short) * CORETONE_BUFFER_LEN * 4;
      bupchip_buffer = (short*)malloc(bufferSize);
      if(!bupchip_buffer)
      {
#ifdef A7800_LOGS
         EMU_LOG("[A7800][BUPCHIP] buffer alloc failed (%u bytes)\n", (unsigned)bufferSize);
#endif
         return false;
      }
      memset(bupchip_buffer, 0, bufferSize);
   }

   if(!bupchip_songs)
   {
      bupchip_songs = (BupchipFileContents*)malloc(sizeof(BupchipFileContents) * 32);
      if(!bupchip_songs)
      {
#ifdef A7800_LOGS
         EMU_LOG("[A7800][BUPCHIP] songs alloc failed (%u bytes)\n",
                 (unsigned)(sizeof(BupchipFileContents) * 32));
#endif
         return false;
      }
      memset(bupchip_songs, 0, sizeof(BupchipFileContents) * 32);
   }

   return true;
}

static void bupchip_ReplaceChar(char *string, char character, char replacement)
{
   while(true)
   {
      if (!(string = strchr(string, character)))
         return;
      *string = replacement;
      string++;
   }
}

int bupchip_InitFromCDF(const char** cdf, size_t* cdfSize, const char *workingDir)
{
   size_t fileIndex;
   char *line;
   BupchipFileContents fileData[34];
   size_t songIndex       = 0;
   uint32_t fileDataCount = 0;

   if(!bupchip_EnsureAllocated())
      return 0;

   while(fileDataCount < sizeof(fileData) / sizeof(fileData[0]) &&
      (line = cartridge_GetNextNonemptyLine(cdf, cdfSize)) != NULL)
   {
#ifndef _WIN32
      /* CDF files always use Windows paths.
         Convert to Unix-style if necessary. */
      bupchip_ReplaceChar(line, '\\', '/');
#endif

      if(!cartridge_ReadFile(&fileData[fileDataCount].data, &fileData[fileDataCount].size, line, workingDir))
      {
         free(line);
         goto err;
      }
      free(line);
      fileDataCount++;
   }

   if(fileDataCount < 2)
      goto err;

   bupchip_sample_data     = fileData[0].data;
   bupchip_instrument_data = fileData[1].data;

   if(ct_init(bupchip_sample_data, bupchip_instrument_data) != 0)
      goto err;

   for(songIndex = 0; songIndex < fileDataCount - 2; songIndex++)
      bupchip_songs[songIndex] = fileData[songIndex + 2];

   bupchip_song_count = (uint8_t)(fileDataCount - 2);
   return 1;

err:
   for(fileIndex = 0; fileIndex < fileDataCount; fileIndex++)
   {
      free(fileData[fileIndex].data);
      fileData[fileIndex].data = NULL;
   }

   bupchip_song_count      = 0;
   bupchip_instrument_data = NULL;
   bupchip_sample_data     = NULL;
   return 0;
}

void bupchip_Stop(void)
{
   bupchip_flags &= ~BUPCHIP_FLAGS_PLAYING;
   ct_stopMusic();
}

void bupchip_Play(unsigned char song)
{
   if(song >= bupchip_song_count)
   {
      bupchip_Stop();
      return;
   }

   bupchip_flags |= BUPCHIP_FLAGS_PLAYING;
   bupchip_current_song = song;
   ct_playMusic(bupchip_songs[bupchip_current_song].data);
}

void bupchip_Pause(void)
{
   bupchip_flags |= BUPCHIP_FLAGS_PAUSED;
   ct_pause();
}

void bupchip_Resume(void)
{
   bupchip_flags &= ~BUPCHIP_FLAGS_PAUSED;
   ct_resume();
}

void bupchip_SetVolume(uint8_t volume)
{
   int attenuation;
   bupchip_volume = volume & 0x1f;

   /* This matches BupSystem. */
   attenuation = volume << 2;
   if((volume & 1) != 0)
      attenuation += 0x3;

   ct_attenMusic(attenuation);
}

void bupchip_ProcessAudioCommand(unsigned char data)
{
   switch(data & 0xc0)
   {
   case 0:
      switch(data)
      {
      case 0:
         bupchip_flags  = 0;
         bupchip_volume = 0x1f;
         ct_stopAll();
         ct_resume();
         ct_attenMusic(127);
         break;

      case 2:
         bupchip_Resume();
         break;

      case 3:
         bupchip_Pause();
         break;
      }
      break;

   case 0x40:
      bupchip_Stop();
      break;

   case 0x80:
      bupchip_Play(data & 0x1f);
      break;

   case 0xc0:
      bupchip_SetVolume(data);
      break;
   }
}

void bupchip_Process(unsigned tick)
{
   if(!bupchip_buffer)
      return;

   ct_update(&bupchip_buffer[tick * CORETONE_BUFFER_LEN]);
}

void bupchip_Release(void)
{
   int i;

   for(i = 0; i < bupchip_song_count; i++)
   {
      free(bupchip_songs[i].data);
      bupchip_songs[i].data = NULL;
      bupchip_songs[i].size = 0;
   }

   free(bupchip_instrument_data);
   bupchip_instrument_data = NULL;

   free(bupchip_sample_data);
   bupchip_sample_data = NULL;

   bupchip_song_count = 0;
   bupchip_flags = 0;
   bupchip_current_song = 0;
}

void bupchip_StateLoaded(void)
{
   ct_stopAll();

   if((bupchip_flags & BUPCHIP_FLAGS_PLAYING) == 0)
      return;

   ct_playMusic(bupchip_songs[bupchip_current_song].data);

   if((bupchip_flags & BUPCHIP_FLAGS_PAUSED) != 0)
      ct_pause();
   else
      ct_resume();

   bupchip_SetVolume(bupchip_volume);
}

void bupchip_Shutdown(void)
{
   bupchip_Release();

   if(bupchip_songs)
   {
      free(bupchip_songs);
      bupchip_songs = NULL;
   }

   if(bupchip_buffer)
   {
      free(bupchip_buffer);
      bupchip_buffer = NULL;
   }
}
