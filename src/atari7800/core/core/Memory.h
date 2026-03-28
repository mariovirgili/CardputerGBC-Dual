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
 * Memory.h
 * ----------------------------------------------------------------------------
 */
#ifndef MEMORY_H
#define MEMORY_H

#define MEMORY_SIZE 65536
#define MEMORY_ROM_FLAGS_SIZE (MEMORY_SIZE / 8)
#define MEMORY_SOUPER_EXRAM_SIZE 32768

#include <stdint.h>
#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool memory_EnsureAllocated(void);
extern bool memory_IsReady(void);
extern void memory_Shutdown(void);
extern void memory_Reset(void);
extern uint8_t memory_Read(uint16_t address);
extern void memory_Write(uint16_t address, uint8_t data);
extern void memory_WriteROM(uint16_t address, uint16_t size, const uint8_t* data);
extern void memory_ClearROM(uint16_t address, uint16_t size);
extern uint16_t memory_souper_GetRamAddress(uint16_t address);

extern uint8_t* memory_ram;
extern uint8_t* memory_rom;
extern uint8_t* memory_souper_ram;

#ifdef ESP_PLATFORM
/* Non-NULL when memory_ram was allocated as a single contiguous 64 KB block.
 * In that case memory_pages[i] = memory_flat_buf + i*MEMORY_PAGE_SIZE, giving
 * the page table better D-cache locality even though mem_rd still uses it. */
extern uint8_t* memory_flat_buf;
#endif

#ifdef ESP_PLATFORM
/* On ESP32-S3 without PSRAM the 64 KB address space is split into
 * 4 × 16 KB pages so each piece fits in the fragmented internal heap. */
#define MEMORY_PAGE_BITS  14
#define MEMORY_PAGE_SIZE  (1u << MEMORY_PAGE_BITS)   /* 16384 */
#define MEMORY_PAGE_MASK  (MEMORY_PAGE_SIZE - 1)
#define MEMORY_NUM_PAGES  (MEMORY_SIZE / MEMORY_PAGE_SIZE)  /* 4 */

extern uint8_t* memory_pages[MEMORY_NUM_PAGES];

static inline uint8_t mem_rd(uint16_t addr) {
   return memory_pages[addr >> MEMORY_PAGE_BITS][addr & MEMORY_PAGE_MASK];
}
static inline void mem_wr(uint16_t addr, uint8_t val) {
   memory_pages[addr >> MEMORY_PAGE_BITS][addr & MEMORY_PAGE_MASK] = val;
}
static inline uint8_t* mem_ptr(uint16_t addr) {
   return &memory_pages[addr >> MEMORY_PAGE_BITS][addr & MEMORY_PAGE_MASK];
}

/* ------------------------------------------------------------------ */
/* Fast inline read for the Sally (6502) hot loop.                    */
/* Handles the INTIM/INTFLG side-effect (clear bit 7 of INTFLG on    */
/* read) inline and returns mem_rd() for every other address.         */
/* ------------------------------------------------------------------ */
/* INTIM = 644, INTFLG = 645; mirrors at 646, 647 (Equates.h) */
static inline uint8_t memory_ReadFast(uint16_t address) {
   if (__builtin_expect((unsigned)(address - 644u) < 4u, 0)) {
      memory_pages[0][645] &= 0x7f;              /* clear INTFLG bit 7 */
      return memory_pages[0][(address & 1) ? 645 : 644];
   }
   return mem_rd(address);
}

#else  /* !ESP_PLATFORM */

#define mem_rd(addr)       (memory_ram[(addr)])
#define mem_wr(addr, val)  (memory_ram[(addr)] = (val))
#define mem_ptr(addr)      (&memory_ram[(addr)])
#endif

#ifdef __cplusplus
}
#endif

#endif
