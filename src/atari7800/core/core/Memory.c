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
 * Memory.c
 * ----------------------------------------------------------------------------
 */
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "Memory.h"
#include "Equates.h"
#include "Bios.h"
#include "Cartridge.h"
#include "Tia.h"
#include "Riot.h"

#ifdef ESP_PLATFORM
uint8_t* memory_pages[MEMORY_NUM_PAGES] = {0};
uint8_t* memory_flat_buf = NULL; /* non-NULL when backed by one 64 KB block */
uint8_t* memory_ram      = NULL; /* always = memory_pages[0] when allocated */
#else
uint8_t* memory_ram = NULL;
#endif
uint8_t* memory_rom = NULL;
uint8_t* memory_souper_ram = NULL;

static bool memory_IsROMAddress(uint16_t address)
{
   return (memory_rom[address >> 3] & (1u << (address & 7))) != 0;
}

static void memory_SetROMAddress(uint16_t address, bool isRom)
{
   uint8_t* entry = &memory_rom[address >> 3];
   const uint8_t mask = (uint8_t)(1u << (address & 7));

   if(isRom)
      *entry |= mask;
   else
      *entry &= (uint8_t)~mask;
}

static void* memory_Alloc(size_t size, bool preferInternal)
{
#ifdef ESP_PLATFORM
   void* ptr = NULL;

   if(preferInternal)
      ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

   if(!ptr)
      ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

   if(!ptr)
      ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);

   return ptr;
#else
   (void)preferInternal;
   return malloc(size);
#endif
}

static bool memory_EnsureSouperAllocated(void)
{
   if(!memory_souper_ram)
      memory_souper_ram = (uint8_t*)memory_Alloc(MEMORY_SOUPER_EXRAM_SIZE, false);

   return memory_souper_ram != NULL;
}

bool memory_EnsureAllocated(void)
{
#ifdef ESP_PLATFORM
   if(!memory_pages[0])
   {
      /* Try a single contiguous 64 KB block first.  When it succeeds all four
       * pages alias into it, giving the D-cache a single hot 64 KB region.
       * On this hardware (no PSRAM, ~131 KB largest free block) the attempt
       * almost always fails once MARIA has been allocated, so the paged
       * fallback is the expected path — the code is here for future benefit
       * if heap layout improves. */
      memory_flat_buf = (uint8_t*)memory_Alloc(MEMORY_SIZE, true);
      if(memory_flat_buf)
      {
         int i;
#ifdef A7800_LOGS
         EMU_LOG("[A7800][MEM] flat 64KB alloc OK, aliasing pages\n");
#endif
         for(i = 0; i < MEMORY_NUM_PAGES; i++)
            memory_pages[i] = memory_flat_buf + (size_t)i * MEMORY_PAGE_SIZE;
      }
      else
      {
         /* Flat allocation failed — fall back to four independent 16 KB pages. */
         int i;
         for(i = 0; i < MEMORY_NUM_PAGES; i++)
         {
            memory_pages[i] = (uint8_t*)memory_Alloc(MEMORY_PAGE_SIZE, true);
            if(!memory_pages[i])
            {
#ifdef A7800_LOGS
               EMU_LOG("[A7800][MEM] page %d alloc failed\n", i);
#endif
               return false;
            }
         }
#ifdef A7800_LOGS
         EMU_LOG("[A7800][MEM] alloc ram 4x%u bytes\n", (unsigned)MEMORY_PAGE_SIZE);
#endif
      }
      memory_ram = memory_pages[0];
   }

   if(!memory_rom)
      memory_rom = (uint8_t*)memory_Alloc(MEMORY_ROM_FLAGS_SIZE, false);

   if(!memory_rom)
      return false;
#else
   if(!memory_ram)
      memory_ram = (uint8_t*)memory_Alloc(MEMORY_SIZE, true);

   if(!memory_rom)
      memory_rom = (uint8_t*)memory_Alloc(MEMORY_ROM_FLAGS_SIZE, false);

   if(!memory_ram || !memory_rom)
      return false;
#endif

   return true;
}

bool memory_IsReady(void)
{
#ifdef ESP_PLATFORM
   int i;
   for(i = 0; i < MEMORY_NUM_PAGES; i++)
      if(!memory_pages[i]) return false;
   return memory_rom != NULL;
#else
   return memory_ram != NULL && memory_rom != NULL;
#endif
}

void memory_Shutdown(void)
{
#ifdef ESP_PLATFORM
   {
      int i;
      if(memory_flat_buf)
      {
         free(memory_flat_buf);
         memory_flat_buf = NULL;
      }
      else
      {
         for(i = 0; i < MEMORY_NUM_PAGES; i++)
         {
            if(memory_pages[i])
               free(memory_pages[i]);
         }
      }
      for(i = 0; i < MEMORY_NUM_PAGES; i++)
         memory_pages[i] = NULL;
      memory_ram = NULL;
   }
#else
   if(memory_ram)
   {
      free(memory_ram);
      memory_ram = NULL;
   }
#endif

   if(memory_rom)
   {
      free(memory_rom);
      memory_rom = NULL;
   }

   if(memory_souper_ram)
   {
      free(memory_souper_ram);
      memory_souper_ram = NULL;
   }
}

void memory_Reset(void)
{
   if(!memory_EnsureAllocated())
      return;

#ifdef ESP_PLATFORM
   {
      int i;
      for(i = 0; i < MEMORY_NUM_PAGES; i++)
         memset(memory_pages[i], 0, MEMORY_PAGE_SIZE);
   }
#else
   memset(memory_ram, 0, MEMORY_SIZE);
#endif
   memset(memory_rom, 0xFF, MEMORY_ROM_FLAGS_SIZE);
   memset(memory_rom, 0x00, 16384 / 8);

   if(cartridge_type == CARTRIDGE_TYPE_SOUPER && memory_EnsureSouperAllocated())
      memset(memory_souper_ram, 0, MEMORY_SOUPER_EXRAM_SIZE);
}

uint16_t memory_souper_GetRamAddress(uint16_t address)
{
  uint8_t page = (address - 0x4000) >> 12;
  if((cartridge_souper_mode & CARTRIDGE_SOUPER_MODE_EXS) != 0)
  {
    if(address >= 0x6000 && address < 0x7000)
      page = cartridge_souper_ram_page_bank[0];
    else if(address >= 0x7000 && address < 0x8000)
      page = cartridge_souper_ram_page_bank[1];
  }
  return (address & 0x0fff) | ((uint16_t)page << 12);
}

uint8_t memory_Read(uint16_t address)
{
   if(!memory_IsReady())
      return 0;

   switch ( address )
   {
      case INTIM:
      case INTIM | 0x2:
         memory_ram[INTFLG] &= 0x7f;
         return memory_ram[INTIM];
      case INTFLG:
      case INTFLG | 0x2:
         memory_ram[INTFLG] &= 0x7f;
         return memory_ram[INTFLG];
      default:
         if(cartridge_type == CARTRIDGE_TYPE_SOUPER && address >= 0x4000 && address < 0x8000)
         {
            if(!memory_souper_ram)
               return 0;
            return memory_souper_ram[memory_souper_GetRamAddress(address)];
         }
         break;
   }

   return mem_rd(address);
}

void memory_Write(uint16_t address, uint8_t data)
{
   if(!memory_IsReady())
      return;

   if(!memory_IsROMAddress(address))
   {
      switch(address)
      {
         case WSYNC:
            if(!(cartridge_flags & 128))
               memory_ram[WSYNC] = true;
            break;
         case INPTCTRL:
            if(data == 22 && cartridge_IsLoaded())
               cartridge_Store();
            else if(data == 2 && bios_enabled)
               bios_Store();
            break;
         case INPT0:
         case INPT1:
         case INPT2:
         case INPT3:
         case INPT4:
         case INPT5:
            break;
         case AUDC0:
            tia_SetRegister(AUDC0, data);
            break;
         case AUDC1:
            tia_SetRegister(AUDC1, data);
            break;
         case AUDF0:
            tia_SetRegister(AUDF0, data);
            break;
         case AUDF1:
            tia_SetRegister(AUDF1, data);
            break;
         case AUDV0:
            tia_SetRegister(AUDV0, data);
            break;
         case AUDV1:
            tia_SetRegister(AUDV1, data);
            break;
         case SWCHA:	/*gdement:  Writing here actually writes to DRA inside the RIOT chip.
                       This value only indirectly affects output of SWCHA.  Ditto for SWCHB.*/
            riot_SetDRA(data);
            break;
         case SWCHB:
            riot_SetDRB(data);
            break;
         case TIM1T:
         case TIM1T | 0x8:
            riot_SetTimer(TIM1T, data);
            break;
         case TIM8T:
         case TIM8T | 0x8:
            riot_SetTimer(TIM8T, data);
            break;
         case TIM64T:
         case TIM64T | 0x8:
            riot_SetTimer(TIM64T, data);
            break;
         case T1024T:
         case T1024T | 0x8:
            riot_SetTimer(T1024T, data);
            break;
         default:
            if(cartridge_type == CARTRIDGE_TYPE_SOUPER && address >= 0x4000 && address < 0x8000)
            {
               if(!memory_souper_ram)
                  break;
               memory_souper_ram[memory_souper_GetRamAddress(address)] = data;
               break;
            }
            mem_wr(address, data);
            if(address >= 8256 && address <= 8447)
               mem_wr(address - 8192, data);
            else if(address >= 8512 && address <= 8702)
               mem_wr(address - 8192, data);
            else if(address >= 64 && address <= 255)
               mem_wr(address + 8192, data);
            else if(address >= 320 && address <= 511)
               mem_wr(address + 8192, data);
            break;
            /*TODO: gdement:  test here for debug port.  Don't put it in the switch because that will change behavior.*/
      }
   }
   else
      cartridge_Write(address, data);
}

void memory_WriteROM(uint16_t address, uint16_t size, const uint8_t* data)
{
   uint32_t index;

   if(!memory_EnsureAllocated())
      return;

   if((address + size) <= MEMORY_SIZE && data != NULL)
   {
#ifdef ESP_PLATFORM
      /* Page-aware bulk copy */
      {
         uint16_t remaining = size;
         uint16_t src_off = 0;
         uint16_t addr = address;
         while(remaining > 0)
         {
            uint16_t page = addr >> MEMORY_PAGE_BITS;
            uint16_t off  = addr & MEMORY_PAGE_MASK;
            uint16_t chunk = MEMORY_PAGE_SIZE - off;
            if(chunk > remaining) chunk = remaining;
            memcpy(&memory_pages[page][off], &data[src_off], chunk);
            addr += chunk;
            src_off += chunk;
            remaining -= chunk;
         }
      }
#else
      memcpy(&memory_ram[address], data, size);
#endif
      for(index = 0; index < size; index++)
         memory_SetROMAddress((uint16_t)(address + index), true);
   }
}

void memory_ClearROM(uint16_t address, uint16_t size)
{
   uint32_t index;

   if(!memory_EnsureAllocated())
      return;

   if((address + size) <= MEMORY_SIZE)
   {
#ifdef ESP_PLATFORM
      /* Page-aware bulk clear */
      {
         uint16_t remaining = size;
         uint16_t addr = address;
         while(remaining > 0)
         {
            uint16_t page = addr >> MEMORY_PAGE_BITS;
            uint16_t off  = addr & MEMORY_PAGE_MASK;
            uint16_t chunk = MEMORY_PAGE_SIZE - off;
            if(chunk > remaining) chunk = remaining;
            memset(&memory_pages[page][off], 0, chunk);
            addr += chunk;
            remaining -= chunk;
         }
      }
#else
      memset(&memory_ram[address], 0, size);
#endif
      for(index = 0; index < size; index++)
         memory_SetROMAddress((uint16_t)(address + index), false);
   }
}
