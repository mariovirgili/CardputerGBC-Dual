/*
Gwenesis : Genesis & megadrive Emulator.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

__author__ = "bzhxx"
__contact__ = "https://github.com/bzhxx"
__license__ = "GPLv3"

*/
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "m68k.h"

#include "ym2612.h"
#include "z80inst.h"
#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_savestate.h"

uint8_t *SRAM = NULL;
uint8_t SRAM_ENABLED = 0;
uint32_t SRAM_START = 0;
uint32_t SRAM_END = 0;
uint32_t SRAM_SIZE = 0;

#if GNW_TARGET_MARIO !=0 || GNW_TARGET_ZELDA!=0
  #pragma GCC optimize("Ofast")
#endif

#define BUS_DISABLE_LOGGING 1

#if !BUS_DISABLE_LOGGING && EMU_LOG_MASTER_ENABLED && defined(MD_RENDER_LOGS)
#include <stdarg.h>
void bus_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s] vc:%04x hc:%04x hv:%04x ", frame_counter, scan_line, subs,gwenesis_vdp_vcounter(),gwenesis_vdp_hcounter(),gwenesis_vdp_hvcounter());

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
	#define bus_log(...)  do {} while(0)
#endif

#if MD_BUS_PROBE_LOGS_ENABLED
typedef struct MdBusProbeStats {
  uint32_t readTotal;
  uint32_t writeTotal;
  uint32_t r8;
  uint32_t r16;
  uint32_t w8;
  uint32_t w16;
  uint32_t rom;
  uint32_t ram;
  uint32_t vdp;
  uint32_t io;
  uint32_t z80Ram;
  uint32_t z80Ctrl;
  uint32_t ym2612;
  uint32_t psg;
  uint32_t bank;
  uint32_t tmss;
  uint32_t none;
  uint32_t readRom;
  uint32_t readRam;
  uint32_t readVdp;
  uint32_t readIo;
  uint32_t readZ80Ram;
  uint32_t readZ80Ctrl;
  uint32_t readYm2612;
  uint32_t readPsg;
  uint32_t readBank;
  uint32_t readTmss;
  uint32_t readNone;
  uint32_t writeRom;
  uint32_t writeRam;
  uint32_t writeVdp;
  uint32_t writeIo;
  uint32_t writeZ80Ram;
  uint32_t writeZ80Ctrl;
  uint32_t writeYm2612;
  uint32_t writePsg;
  uint32_t writeBank;
  uint32_t writeTmss;
  uint32_t writeNone;
  uint32_t vdpDataRead;
  uint32_t vdpCtrlRead;
  uint32_t vdpHvRead;
  uint32_t vdpOtherRead;
  uint32_t vdpDataWrite;
  uint32_t vdpCtrlWrite;
  uint32_t vdpOtherWrite;
  uint32_t fallbackRead16;
  uint32_t fallbackWrite16;
} MdBusProbeStats;

static MdBusProbeStats s_mdBusProbe;

void gwenesis_bus_probe_reset(void)
{
  memset(&s_mdBusProbe, 0, sizeof(s_mdBusProbe));
}

static inline void md_bus_probe_record(unsigned int mapped, int isWrite, int bits)
{
  if (isWrite) {
    ++s_mdBusProbe.writeTotal;
    if (bits == 8) ++s_mdBusProbe.w8;
    else ++s_mdBusProbe.w16;
  } else {
    ++s_mdBusProbe.readTotal;
    if (bits == 8) ++s_mdBusProbe.r8;
    else ++s_mdBusProbe.r16;
  }

  switch (mapped) {
  case ROM_ADDR:
  case ROM_ADDR_MIRROR:
    ++s_mdBusProbe.rom;
    if (isWrite) ++s_mdBusProbe.writeRom;
    else ++s_mdBusProbe.readRom;
    break;
  case RAM_ADDR:
    ++s_mdBusProbe.ram;
    if (isWrite) ++s_mdBusProbe.writeRam;
    else ++s_mdBusProbe.readRam;
    break;
  case VDP_ADDR:
    ++s_mdBusProbe.vdp;
    if (isWrite) ++s_mdBusProbe.writeVdp;
    else ++s_mdBusProbe.readVdp;
    break;
  case IO_CTRL:
    ++s_mdBusProbe.io;
    if (isWrite) ++s_mdBusProbe.writeIo;
    else ++s_mdBusProbe.readIo;
    break;
  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    ++s_mdBusProbe.z80Ram;
    if (isWrite) ++s_mdBusProbe.writeZ80Ram;
    else ++s_mdBusProbe.readZ80Ram;
    break;
  case Z80_CTRL:
    ++s_mdBusProbe.z80Ctrl;
    if (isWrite) ++s_mdBusProbe.writeZ80Ctrl;
    else ++s_mdBusProbe.readZ80Ctrl;
    break;
  case Z80_YM2612_ADDR:
    ++s_mdBusProbe.ym2612;
    if (isWrite) ++s_mdBusProbe.writeYm2612;
    else ++s_mdBusProbe.readYm2612;
    break;
  case Z80_SN76489_ADDR:
    ++s_mdBusProbe.psg;
    if (isWrite) ++s_mdBusProbe.writePsg;
    else ++s_mdBusProbe.readPsg;
    break;
  case Z80_BANK_ADDR:
    ++s_mdBusProbe.bank;
    if (isWrite) ++s_mdBusProbe.writeBank;
    else ++s_mdBusProbe.readBank;
    break;
  case TMSS_CTRL:
    ++s_mdBusProbe.tmss;
    if (isWrite) ++s_mdBusProbe.writeTmss;
    else ++s_mdBusProbe.readTmss;
    break;
  default:
    ++s_mdBusProbe.none;
    if (isWrite) ++s_mdBusProbe.writeNone;
    else ++s_mdBusProbe.readNone;
    break;
  }
}

static inline void md_bus_probe_vdp_port(unsigned int address, int isWrite)
{
  const unsigned int port = address & 0x0Eu;
  if (isWrite) {
    if (port == 0x00u || port == 0x02u) ++s_mdBusProbe.vdpDataWrite;
    else if (port == 0x04u || port == 0x06u) ++s_mdBusProbe.vdpCtrlWrite;
    else ++s_mdBusProbe.vdpOtherWrite;
  } else {
    if (port == 0x00u || port == 0x02u) ++s_mdBusProbe.vdpDataRead;
    else if (port == 0x04u || port == 0x06u) ++s_mdBusProbe.vdpCtrlRead;
    else if (port == 0x08u || port == 0x0Au) ++s_mdBusProbe.vdpHvRead;
    else ++s_mdBusProbe.vdpOtherRead;
  }
}

static inline void md_bus_probe_fallback_read16(void)
{
  ++s_mdBusProbe.fallbackRead16;
}

static inline void md_bus_probe_fallback_write16(void)
{
  ++s_mdBusProbe.fallbackWrite16;
}

void gwenesis_bus_probe_log_and_reset(void)
{
  const uint32_t total = s_mdBusProbe.readTotal + s_mdBusProbe.writeTotal;
  if (total == 0) return;

  MD_BUS_LOG("ops=%lu r/w=%lu/%lu r8/16=%lu/%lu w8/16=%lu/%lu area rom/ram/vdp/io=%lu/%lu/%lu/%lu z80 ram/ctrl/ym/psg/bank=%lu/%lu/%lu/%lu/%lu tmss/none=%lu/%lu rd rom/ram/vdp/io=%lu/%lu/%lu/%lu z80 ram/ctrl/ym/psg/bank=%lu/%lu/%lu/%lu/%lu tmss/none=%lu/%lu wr rom/ram/vdp/io=%lu/%lu/%lu/%lu z80 ram/ctrl/ym/psg/bank=%lu/%lu/%lu/%lu/%lu tmss/none=%lu/%lu vdp rd data/ctrl/hv/other=%lu/%lu/%lu/%lu wr data/ctrl/other=%lu/%lu/%lu fallback r16/w16=%lu/%lu",
             (unsigned long)total,
             (unsigned long)s_mdBusProbe.readTotal,
             (unsigned long)s_mdBusProbe.writeTotal,
             (unsigned long)s_mdBusProbe.r8,
             (unsigned long)s_mdBusProbe.r16,
             (unsigned long)s_mdBusProbe.w8,
             (unsigned long)s_mdBusProbe.w16,
             (unsigned long)s_mdBusProbe.rom,
             (unsigned long)s_mdBusProbe.ram,
             (unsigned long)s_mdBusProbe.vdp,
             (unsigned long)s_mdBusProbe.io,
             (unsigned long)s_mdBusProbe.z80Ram,
             (unsigned long)s_mdBusProbe.z80Ctrl,
             (unsigned long)s_mdBusProbe.ym2612,
             (unsigned long)s_mdBusProbe.psg,
             (unsigned long)s_mdBusProbe.bank,
             (unsigned long)s_mdBusProbe.tmss,
             (unsigned long)s_mdBusProbe.none,
             (unsigned long)s_mdBusProbe.readRom,
             (unsigned long)s_mdBusProbe.readRam,
             (unsigned long)s_mdBusProbe.readVdp,
             (unsigned long)s_mdBusProbe.readIo,
             (unsigned long)s_mdBusProbe.readZ80Ram,
             (unsigned long)s_mdBusProbe.readZ80Ctrl,
             (unsigned long)s_mdBusProbe.readYm2612,
             (unsigned long)s_mdBusProbe.readPsg,
             (unsigned long)s_mdBusProbe.readBank,
             (unsigned long)s_mdBusProbe.readTmss,
             (unsigned long)s_mdBusProbe.readNone,
             (unsigned long)s_mdBusProbe.writeRom,
             (unsigned long)s_mdBusProbe.writeRam,
             (unsigned long)s_mdBusProbe.writeVdp,
             (unsigned long)s_mdBusProbe.writeIo,
             (unsigned long)s_mdBusProbe.writeZ80Ram,
             (unsigned long)s_mdBusProbe.writeZ80Ctrl,
             (unsigned long)s_mdBusProbe.writeYm2612,
             (unsigned long)s_mdBusProbe.writePsg,
             (unsigned long)s_mdBusProbe.writeBank,
             (unsigned long)s_mdBusProbe.writeTmss,
             (unsigned long)s_mdBusProbe.writeNone,
             (unsigned long)s_mdBusProbe.vdpDataRead,
             (unsigned long)s_mdBusProbe.vdpCtrlRead,
             (unsigned long)s_mdBusProbe.vdpHvRead,
             (unsigned long)s_mdBusProbe.vdpOtherRead,
             (unsigned long)s_mdBusProbe.vdpDataWrite,
             (unsigned long)s_mdBusProbe.vdpCtrlWrite,
             (unsigned long)s_mdBusProbe.vdpOtherWrite,
             (unsigned long)s_mdBusProbe.fallbackRead16,
             (unsigned long)s_mdBusProbe.fallbackWrite16);
  gwenesis_bus_probe_reset();
}
#else
void gwenesis_bus_probe_reset(void) {}
void gwenesis_bus_probe_log_and_reset(void) {}
#define md_bus_probe_record(mapped, isWrite, bits) do { (void)(mapped); } while (0)
#define md_bus_probe_vdp_port(address, isWrite) do { (void)(address); } while (0)
#define md_bus_probe_fallback_read16() do {} while (0)
#define md_bus_probe_fallback_write16() do {} while (0)
#endif

// Setup M68k memories ROM & RAM
#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

#include "rom_manager.h"
unsigned char *M68K_RAM=(void *)(uint32_t)(0); // 68K RAM 
#else

unsigned char *ROM_DATA; // 68K Main Program (uncompressed)
unsigned int ROM_SIZE = 0;
unsigned int ROM_ADDR_MASK = 0;
unsigned int ROM_ADDR_IS_POW2 = 0;
unsigned char* M68K_RAM = NULL;
#endif


// Setup Z80 Memory
unsigned char* ZRAM = NULL; // Z80 RAM
unsigned char TMSS[0x4];
extern unsigned short gwenesis_vdp_status;
static int s_region_pal = 0;
static int s_region_refresh_rate = GWENESIS_REFRESH_RATE_NTSC;
static int s_region_audio_rate = GWENESIS_AUDIO_FREQ_NTSC;
static int s_region_audio_divisor = GWENESIS_AUDIO_DIVISOR_NTSC;
static int s_region_lines_per_frame = LINES_PER_FRAME_NTSC;
static const char *s_region_name = "NTSC-U";

// TMSS
int tmss_state = 0;
int tmss_count = 0;

static void apply_console_region(const char *name, int io_reg0, int pal)
{
  s_region_name = name;
  s_region_pal = pal ? 1 : 0;
  s_region_refresh_rate = s_region_pal ? GWENESIS_REFRESH_RATE_PAL : GWENESIS_REFRESH_RATE_NTSC;
  s_region_audio_rate = s_region_pal ? GWENESIS_AUDIO_FREQ_PAL : GWENESIS_AUDIO_FREQ_NTSC;
  s_region_audio_divisor = s_region_pal ? GWENESIS_AUDIO_DIVISOR_PAL : GWENESIS_AUDIO_DIVISOR_NTSC;
  s_region_lines_per_frame = s_region_pal ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
  gwenesis_io_set_reg(0, io_reg0);
  gwenesis_region_apply_vdp_status();
}

int gwenesis_region_is_pal(void)
{
  return s_region_pal;
}

int gwenesis_region_refresh_rate(void)
{
  return s_region_refresh_rate;
}

int gwenesis_region_audio_rate(void)
{
  return s_region_audio_rate;
}

int gwenesis_region_audio_divisor(void)
{
  return s_region_audio_divisor;
}

int gwenesis_region_lines_per_frame(void)
{
  return s_region_lines_per_frame;
}

const char *gwenesis_region_name(void)
{
  return s_region_name;
}

void gwenesis_region_apply_vdp_status(void)
{
  if (s_region_pal) {
    gwenesis_vdp_status |= STATUS_PAL;
  } else {
    gwenesis_vdp_status &= (unsigned short)~STATUS_PAL;
  }
}

/******************************************************************************
 *
 *   Load a Sega Genesis Cartridge into CPU Memory
 *
 ******************************************************************************/


#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

void load_cartridge()
{
    // Clear all volatile memory
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    // Set Z80 Memory as Z80_RAM
    z80_set_memory(ZRAM);

    z80_pulse_reset();

    set_region();

}
#else

void load_cartridge(unsigned char *buffer, size_t size)
{
    ROM_SIZE = (unsigned int)size;
    ROM_ADDR_IS_POW2 = (ROM_SIZE != 0u) && ((ROM_SIZE & (ROM_SIZE - 1u)) == 0u);
    ROM_ADDR_MASK = ROM_ADDR_IS_POW2 ? (ROM_SIZE - 1u) : 0u;
    (void)size; // pas nécessaire ici, gardé si tu veux l’utiliser plus tard

    // Clear RAMs volatiles
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    // Pointe les zones mémoire
    ROM_DATA = buffer;
    z80_set_memory(ZRAM);
    z80_pulse_reset();

    // Détermine la région depuis l’en-tête ROM
    set_region();
    // // Clear all volatile memory
    // memset(M68K_RAM, 0, MAX_RAM_SIZE);
    // memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    // // Set Z80 Memory as ZRAM
    // z80_set_memory(ZRAM);
    // z80_pulse_reset();

    // // Copy file contents to CPU ROM memory
    // #ifdef RETRO_GO
    // ROM_DATA = buffer;
    // #else
    // ROM_DATA = realloc(ROM_DATA, (size & ~0xFFFF) + 0x10000); // 64KB align just in case
    // memcpy(ROM_DATA, buffer, size);
    // #endif

    // // https://github.com/franckverrot/EmulationResources/blob/master/consoles/megadrive/genesis_rom.txt
    // if (ROM_DATA[1] == 0x03 && ROM_DATA[8] == 0xAA && ROM_DATA[9] == 0xBB)
    // {
    //   printf("--SMD de-interleave mode--\n");
    //   memmove(ROM_DATA, ROM_DATA + 512, size - 512);
    //   uint8 *temp = malloc(0x4000);
    //   for (size_t i = 0; i < size; i += 0x4000)
    //   {
    //     memcpy(temp, ROM_DATA + i, 0x4000);
    //     for (size_t j = 0; j < 0x2000; ++j)
    //     {
    //       ROM_DATA[i + (j * 2) + 0] = temp[0x2000 + j];
    //       ROM_DATA[i + (j * 2) + 1] = temp[0x0000 + j];
    //     }
    //   }
    //   free(temp);
    // }

    // #ifdef ROM_SWAP
    // bus_log(__FUNCTION__,"--ROM swap mode--");
    // for (int i=0; i < size;i+=2 )
    // {   
    //     char z = ROM_DATA[i];
    //     ROM_DATA[i]=ROM_DATA[i+1];
    //     ROM_DATA[i+1]=z;
    // }
    // #endif


    // set_region();
}

#endif

/******************************************************************************
 *
 *   Power ON the CPU
 *   Initialize 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void power_on() {
  // Set M68K CPU as original MOTOROLA 68000
  //m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  // Initialize M68K CPU
  m68k_init();
  // Initialize Z80 CPU
  z80_start();
  // Initialize YM2612 chip
  YM2612Init();
  YM2612Config(9);
  YM2612SetDivisor(s_region_audio_divisor);
  // Initialize PSG SN76489 chip
  //CLOCK_NTSC      = 3579545,
  //CLOCK_PAL       = 3546895,
 // CLOCK_NTSC_SMS1 = 3579527

  if (s_region_pal) {
    gwenesis_SN76489_Init(3546895, s_region_audio_rate, s_region_audio_divisor);
  } else {
    gwenesis_SN76489_Init(3579545, s_region_audio_rate, s_region_audio_divisor);
  }

}

/******************************************************************************
 *
 *   Reset the CPU Emulation
 *   Send a pulse reset to 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void reset_emulation() {
  // Send a reset pulse to Z80 CPU
  z80_pulse_reset();
  // Send a reset pulse to Z80 M68K
  m68k_pulse_reset();
  // Send a reset pulse to YM2612 chip
  YM2612ResetChip();
  // Send a reset pulse to SEGA 315-5313 chip
  gwenesis_vdp_reset();
  gwenesis_SN76489_Reset();
}

/******************************************************************************
 *
 *   Set Region
 *   Look at ROM to set console compatible region
 *
 ******************************************************************************/
void set_region()
{    
  /*
    old style : JUE characters
    J : Domestic 60Hz (Asia)
    U : Oversea  60Hz (USA) 
    E : Oversea  50Hz (Europe) 

    new style : 1st character
    bit 0 : +1 Domestic 60Hz (Asia)
    bit 1 : +2 Domestc  50Hz (Asia)
    bit 2:  +4 Oversea  60Hz (USA) 
    bit 3:  +4 Oversea  50Hz (Europe) 
  */

   // extern int mode_pal;

    int country = 0;

    char rom_str[3];

#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
    EMU_LOG("ROM game  : ");
    for (int j=0; j < 48;j++) EMU_LOG("%c",(char)FETCH8ROM(0x150+j));
    EMU_LOG("\n");
#endif

    rom_str[0]=FETCH8ROM(0x1F0);
    rom_str[1]=FETCH8ROM(0x1F1);
    rom_str[2]=FETCH8ROM(0x1F2);

#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
    EMU_LOG("ROM region:%c%c%c (0x%02x 0x%02x 0x%02x)\n", rom_str[0],rom_str[1],rom_str[2],rom_str[0],rom_str[1],rom_str[2]);
#endif

    /* from Gens */
    if (!memcmp(rom_str, "eur", 3)) country |= 8;
    else if (!memcmp(rom_str, "EUR", 3)) country |= 8;
    else if (!memcmp(rom_str, "Europe", 3)) country |= 8;
    else if (!memcmp(rom_str, "jap", 3)) country |= 1;
    else if (!memcmp(rom_str, "JAP", 3)) country |= 1;
    else if (!memcmp(rom_str, "usa", 3)) country |= 4;
    else if (!memcmp(rom_str, "USA", 3)) country |= 4;
    else
    {
      int i;
      unsigned char c;

      /* look for each characters */
      for(i = 0; i < 3; i++)
      {
        c = rom_str[i];

        if (c == 'U') country |= 4;
        else if (c == 'E' || c == 'e' ) country |= 8;
        else if (c == 'J' || c == 'j' ) country |= 1;
        else if (c == 'K' || c == 'k' ) country |= 1;
        else if (c < 16) country |= c;
        else if ((c >= '0') && (c <= '9')) country |= c - '0';
        else if ((c >= 'A') && (c <= 'F')) country |= c - 'A' + 10;
      }
    }
#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
    EMU_LOG("country code=%01x : ",country);
#endif
      /* set default console region (USA > EUROPE > JAPAN) */
      /*
      IO REG0	:	MODE 	VMOD 	DISK 	RSV 	VER3 	VER2 	VER1 	VER0
      MODE (R) 	0: Domestic Model
  	            1: Overseas Model
      VMOD (R) 	0: NTSC CPU clock 7.67 MHz
  	            1: PAL CPU clock 7.60 MHz
      */

    /* USA 60Hz*/
    if (country & 4){
#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
      EMU_LOG("Oversea-NTSC USA 60Hz\n");
#endif
      apply_console_region("NTSC-U", 0x81, 0);
      return;
    }
    /* EUROPE 50Hz */
    if (country & 8){
#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
      EMU_LOG("Oversea-PAL Europe 50Hz\n");
#endif
      apply_console_region("PAL-E", 0xC1, 1);
      return;
    }
    /* set Asia 60HZ */
    if (country & 1){
#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
      EMU_LOG("Domestic-NTSC Asia 60Hz\n");
#endif
      apply_console_region("NTSC-J", 0x01, 0);
      return;
    }
#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
      EMU_LOG("Oversea-NTSC USA 60Hz no detection>> default mode\n");
#endif
      apply_console_region("NTSC-U", 0x81, 0);

}
/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *   68K Access to Z80 Memory
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_z80_address(unsigned int address) {

  unsigned int range = (address & 0xF000);
  switch (range) {
  case 0:
  case 0x1000:
    return Z80_RAM_ADDR;
  case 0x2000:
  case 0x3000:
    return Z80_RAM_ADDR1K;
  case 0x4000:
    return Z80_YM2612_ADDR;
  case 0x6000:
    return Z80_BANK_ADDR;
  case 0x7000:
    return Z80_SN76489_ADDR;
  default:
    bus_log(__FUNCTION__,"no map Z80 %x",address);
    assert(0);
    return NONE;
  }
}

/******************************************************************************
 *
 *   IO memory address mapper
 *   Map all input/output region address for CPU program
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_io_address(unsigned int address)
{
  unsigned int range = (address & 0x1000) ;
  switch (range) {
  case 0:      return IO_CTRL;
  case 0x1000: return Z80_CTRL;
  default:
      // if (address >= 0xa14000 && address < 0xa11404)
      // return (tmss_state == 0) ? TMSS_CTRL : NONE;
      bus_log(__FUNCTION__,"no map io %x",address);

    return NONE;
  }
}

/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *
 ******************************************************************************/

static inline 
unsigned int gwenesis_bus_map_address(unsigned int address) {
  // Mask address page
  unsigned int range = (address & 0xFF0000) >> 16;

  // Check mask and select memory type
  if (range < 0x80) //        ROM ADDRESS 0x000000 - 0x3FFFFF
    return ROM_ADDR;

  else if (range == 0xA0) // Z80 ADDRESS 0xA00000 - 0xA0FFFF
    return gwenesis_bus_map_z80_address(address);


  else if (range == 0xA1) //                  IO ADDRESS  0xA10000 - 0xA1FFFF
    return gwenesis_bus_map_io_address(address);

  else if (range == 0xC0) // VDP ADDRESS 0xC00000 - 0xDFFFFFF
    return VDP_ADDR;
  else if (range >= 0xE0) // RAM ADDRESS 0xE00000 - 0xFFFFFF
    return RAM_ADDR;
  // If not a valid address return 0
  bus_log(__FUNCTION__,"M68K > ?? unnmap address %x", address);
  //assert(0);
  return NONE;
}

#ifndef MD_DIRECT_BUS_DISPATCH
#define MD_DIRECT_BUS_DISPATCH 0
#endif

#ifndef MD_VDP_STATUS_READ_FASTPATH
#define MD_VDP_STATUS_READ_FASTPATH 0
#endif
/******************************************************************************
 *
 *   Main read address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_read_memory_8(unsigned int address) {
 bus_log(__FUNCTION__,"read8  %x", address);
#if MD_DIRECT_BUS_DISPATCH
  const unsigned int range = (address >> 16) & 0xFFu;

  if (range < 0x80u) {
    md_bus_probe_record(ROM_ADDR, 0, 8);
    return FETCH8ROM(address);
  }

  if (range >= 0xE0u) {
    md_bus_probe_record(RAM_ADDR, 0, 8);
    return FETCH8RAM(address);
  }

  if (range == 0xC0u) {
    md_bus_probe_record(VDP_ADDR, 0, 8);
    md_bus_probe_vdp_port(address, 0);
    return gwenesis_vdp_read_memory_8(address);
  }

  if (range == 0xA1u) {
    if ((address & 0x1000u) == 0u) {
      md_bus_probe_record(IO_CTRL, 0, 8);
      return gwenesis_io_read_ctrl(address & 0x1F);
    }
    md_bus_probe_record(Z80_CTRL, 0, 8);
    return z80_read_ctrl(address & 0xFFFF);
  }

  if (range == 0xA0u) {
    switch (address & 0xF000u) {
    case 0x0000u:
    case 0x1000u:
      md_bus_probe_record(Z80_RAM_ADDR, 0, 8);
      return ZRAM[address & 0x1FFF];
    case 0x2000u:
    case 0x3000u:
      md_bus_probe_record(Z80_RAM_ADDR1K, 0, 8);
      return ZRAM[address & 0x1FFF];
    case 0x4000u:
      md_bus_probe_record(Z80_YM2612_ADDR, 0, 8);
      return YM2612Read(m68k_cycles_master());
    case 0x6000u:
      md_bus_probe_record(Z80_BANK_ADDR, 0, 8);
      return 0xFF;
    case 0x7000u:
      md_bus_probe_record(Z80_SN76489_ADDR, 0, 8);
      return 0xFF;
    default:
      md_bus_probe_record(NONE, 0, 8);
      bus_log(__FUNCTION__," default read 8 %x", address);
      return 0x00;
    }
  }

  md_bus_probe_record(NONE, 0, 8);
  bus_log(__FUNCTION__," default read 8 %x", address);
  return 0x00;
#else
  const unsigned int mapped = gwenesis_bus_map_address(address);
  md_bus_probe_record(mapped, 0, 8);

  switch (mapped) {
  
  case VDP_ADDR:
    md_bus_probe_vdp_port(address, 0);
    return gwenesis_vdp_read_memory_8(address);

  case ROM_ADDR:
    return FETCH8ROM(address);

  case RAM_ADDR:
    return FETCH8RAM(address);

  case IO_CTRL:
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
    return z80_read_ctrl(address & 0xFFFF);

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    return ZRAM[address & 0x1FFF];

  case Z80_YM2612_ADDR:
    return YM2612Read(m68k_cycles_master());

  case Z80_SN76489_ADDR:
    return 0xff;

  case Z80_BANK_ADDR:
    return 0xff;

  case TMSS_CTRL:
    bus_log(__FUNCTION__,"TMS");
    if (tmss_state == 0)
      return TMSS[address & 0x4];
    return 0xFF;

  default:
     bus_log(__FUNCTION__," default read 8 %x", address);
    return 0x00;
  }
  return 0x00;
#endif
}

static inline unsigned int gwenesis_bus_read_memory_16(unsigned int address) {
   bus_log(__FUNCTION__,"read16 %x", address);
   unsigned int ret_value;
#if MD_DIRECT_BUS_DISPATCH
   const unsigned int range = (address >> 16) & 0xFFu;

  if (range == 0xC0u) {
    md_bus_probe_record(VDP_ADDR, 0, 16);
    md_bus_probe_vdp_port(address, 0);
    return gwenesis_vdp_read_memory_16(address);
  }

  if (range >= 0xE0u) {
    md_bus_probe_record(RAM_ADDR, 0, 16);
    return FETCH16RAM(address);
  }

  if (range < 0x80u) {
    md_bus_probe_record(ROM_ADDR, 0, 16);
    return FETCH16ROM(address);
  }

  if (range == 0xA1u) {
    if ((address & 0x1000u) == 0u) {
      md_bus_probe_record(IO_CTRL, 0, 16);
      return gwenesis_io_read_ctrl(address & 0x1F);
    }
    md_bus_probe_record(Z80_CTRL, 0, 16);
    address &= 0xFFFFu;
    return (z80_read_ctrl(address) << 8) | z80_read_ctrl(address | 1u);
  }

  if (range == 0xA0u) {
    switch (address & 0xF000u) {
    case 0x0000u:
    case 0x1000u:
      md_bus_probe_record(Z80_RAM_ADDR, 0, 16);
      return ZRAM[address & 0x1FFF] | (ZRAM[address & 0x1FFF] << 8);
    case 0x2000u:
    case 0x3000u:
      md_bus_probe_record(Z80_RAM_ADDR1K, 0, 16);
      return ZRAM[address & 0x1FFF] | (ZRAM[address & 0x1FFF] << 8);
    case 0x4000u:
      md_bus_probe_record(Z80_YM2612_ADDR, 0, 16);
      ret_value = YM2612Read(m68k_cycles_master());
      return ret_value | (ret_value << 8);
    case 0x6000u:
      md_bus_probe_record(Z80_BANK_ADDR, 0, 16);
      return 0xFF;
    case 0x7000u:
      md_bus_probe_record(Z80_SN76489_ADDR, 0, 16);
      return 0xFF;
    default:
      md_bus_probe_record(NONE, 0, 16);
      md_bus_probe_fallback_read16();
      bus_log(__FUNCTION__,"read mem 16 default %x", address);
      return (gwenesis_bus_read_memory_8(address) << 8) |
             gwenesis_bus_read_memory_8(address + 1);
    }
  }

  md_bus_probe_record(NONE, 0, 16);
  md_bus_probe_fallback_read16();
  bus_log(__FUNCTION__,"read mem 16 default %x", address);
  return (gwenesis_bus_read_memory_8(address) << 8) |
         gwenesis_bus_read_memory_8(address + 1);
#else
   const unsigned int mapped = gwenesis_bus_map_address(address);
   md_bus_probe_record(mapped, 0, 16);

  switch (mapped) {

  case VDP_ADDR:
    md_bus_probe_vdp_port(address, 0);
    return gwenesis_vdp_read_memory_16(address);

  case RAM_ADDR:
    return FETCH16RAM(address);

  case ROM_ADDR:
    return FETCH16ROM(address);

  case IO_CTRL:
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
  //  ret_value = z80_read_ctrl(address & 0xFFFF); 
   // return ret_value | ret_value << 8;
    address &=0xFFFF;
        return (z80_read_ctrl(address) << 8) | z80_read_ctrl(address | 1);


  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    return ZRAM[address & 0X1FFF] | (ZRAM[address & 0X1FFF] << 8);

  case Z80_YM2612_ADDR:
    ret_value = YM2612Read(m68k_cycles_master());
    return ret_value | ret_value << 8;


  case Z80_SN76489_ADDR:
    return 0xff;

  case Z80_BANK_ADDR:
    return 0xff;

  default:
    md_bus_probe_fallback_read16();
    bus_log(__FUNCTION__,"read mem 16 default %x", address);
    return (gwenesis_bus_read_memory_8(address) << 8) |
           gwenesis_bus_read_memory_8(address + 1);
  }
  return 0x00;
#endif
}

/******************************************************************************
 *
 *   Main write address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline void gwenesis_bus_write_memory_8(unsigned int address,
                                              unsigned int value) {
  bus_log(__FUNCTION__,"write8  @%x:%x", address,value);
#if MD_DIRECT_BUS_DISPATCH
  const unsigned int range = (address >> 16) & 0xFFu;

  if (range == 0xC0u) {
    md_bus_probe_record(VDP_ADDR, 1, 8);
    md_bus_probe_vdp_port(address, 1);
    gwenesis_vdp_write_memory_16(address & ~1u, (value << 8) | value);
    return;
  }

  if (range >= 0xE0u) {
    md_bus_probe_record(RAM_ADDR, 1, 8);
    WRITE8RAM(address, value);
    return;
  }

  if (range == 0xA1u) {
    if ((address & 0x1000u) == 0u) {
      md_bus_probe_record(IO_CTRL, 1, 8);
      gwenesis_io_write_ctrl(address & 0x1F, value);
      return;
    }
    md_bus_probe_record(Z80_CTRL, 1, 8);
    z80_write_ctrl(address & 0x1FFF, value);
    return;
  }

  if (range == 0xA0u) {
    switch (address & 0xF000u) {
    case 0x0000u:
    case 0x1000u:
      md_bus_probe_record(Z80_RAM_ADDR, 1, 8);
      ZRAM[address & 0x1FFF] = value;
      return;
    case 0x2000u:
    case 0x3000u:
      md_bus_probe_record(Z80_RAM_ADDR1K, 1, 8);
      ZRAM[address & 0x1FFF] = value;
      return;
    case 0x4000u:
      md_bus_probe_record(Z80_YM2612_ADDR, 1, 8);
      bus_log(__FUNCTION__,"CPUZ80PSG8 ,m68kclk= %d", m68k_cycles_master());
      YM2612Write(address & 0x3, value & 0Xff,m68k_cycles_master());
      return;
    case 0x6000u:
      md_bus_probe_record(Z80_BANK_ADDR, 1, 8);
      return;
    case 0x7000u:
      md_bus_probe_record(Z80_SN76489_ADDR, 1, 8);
      bus_log(__FUNCTION__,"CPUZ80FM8  ,m68kclk= %d", m68k_cycles_master());
      gwenesis_SN76489_Write(value & 0Xff, m68k_cycles_master());
      return;
    default:
      md_bus_probe_record(NONE, 1, 8);
      return;
    }
  }

  md_bus_probe_record(NONE, 1, 8);
  return;
#else
  const unsigned int mapped = gwenesis_bus_map_address(address);
  md_bus_probe_record(mapped, 1, 8);

  switch (mapped) {

  case VDP_ADDR:
    md_bus_probe_vdp_port(address, 1);
    gwenesis_vdp_write_memory_16(address & ~1, (value << 8) | value);
    return;

  case RAM_ADDR:
    WRITE8RAM(address, value);
    return;

  case IO_CTRL:
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    z80_write_ctrl(address & 0x1FFF, value);
    return;

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    ZRAM[address & 0x1FFF] = value;
    return;

  case Z80_YM2612_ADDR:
    bus_log(__FUNCTION__,"CPUZ80PSG8 ,m68kclk= %d", m68k_cycles_master());
    YM2612Write(address & 0x3, value & 0Xff,m68k_cycles_master());
    return;

  case Z80_SN76489_ADDR:
    bus_log(__FUNCTION__,"CPUZ80FM8  ,m68kclk= %d", m68k_cycles_master());
    gwenesis_SN76489_Write( value & 0Xff, m68k_cycles_master());
    return;

  case Z80_BANK_ADDR:
  //TODO
    return;

  case TMSS_CTRL:

    if (tmss_state == 0) {
      TMSS[address & 0x4] = value;
      tmss_count++;
      if (tmss_count == 4)
        tmss_state = 1;
    }
    return;



  default:
    //printf("write(%x, %x)\n", address, value);
    return;
  }
  return;
#endif
}

static inline void gwenesis_bus_write_memory_16(unsigned int address,
                                               unsigned int value) {
  bus_log(__FUNCTION__,"write16  @%x:%x", address,value);
#if MD_DIRECT_BUS_DISPATCH
  const unsigned int range = (address >> 16) & 0xFFu;

  if (range == 0xC0u) {
    md_bus_probe_record(VDP_ADDR, 1, 16);
    md_bus_probe_vdp_port(address, 1);
    gwenesis_vdp_write_memory_16(address, value);
    return;
  }

  if (range >= 0xE0u) {
    md_bus_probe_record(RAM_ADDR, 1, 16);
    WRITE16RAM(address, value);
    return;
  }

  if (range == 0xA1u) {
    if ((address & 0x1000u) == 0u) {
      md_bus_probe_record(IO_CTRL, 1, 16);
      gwenesis_io_write_ctrl(address & 0x1F, value);
      return;
    }
    md_bus_probe_record(Z80_CTRL, 1, 16);
    z80_write_ctrl(address & 0xFFFFu, value >> 8);
    return;
  }

  if (range == 0xA0u) {
    switch (address & 0xF000u) {
    case 0x0000u:
    case 0x1000u:
      md_bus_probe_record(Z80_RAM_ADDR, 1, 16);
      ZRAM[address & 0X1FFF] = value >> 8;
      return;
    case 0x2000u:
    case 0x3000u:
      md_bus_probe_record(Z80_RAM_ADDR1K, 1, 16);
      ZRAM[address & 0X1FFF] = value >> 8;
      return;
    case 0x4000u:
      md_bus_probe_record(Z80_YM2612_ADDR, 1, 16);
      bus_log(__FUNCTION__,"CZYM16 ,mclk=%d",  m68k_cycles_master());
      YM2612Write(address & 0x3, value >> 8, m68k_cycles_master());
      return;
    case 0x7000u:
      md_bus_probe_record(Z80_SN76489_ADDR, 1, 16);
      bus_log(__FUNCTION__,"CZSN16 ,mclk=%d", m68k_cycles_master());
      gwenesis_SN76489_Write(value >> 8, m68k_cycles_master());
      return;
    default:
      md_bus_probe_record(NONE, 1, 16);
      md_bus_probe_fallback_write16();
      bus_log(__FUNCTION__,"write mem 16 default %x ", address);
      gwenesis_bus_write_memory_8(address, (value >> 8) & 0xff);
      gwenesis_bus_write_memory_8(address + 1, value & 0xff);
      return;
    }
  }

  md_bus_probe_record(NONE, 1, 16);
  md_bus_probe_fallback_write16();
  bus_log(__FUNCTION__,"write mem 16 default %x ", address);
  gwenesis_bus_write_memory_8(address, (value >> 8) & 0xff);
  gwenesis_bus_write_memory_8(address + 1, value & 0xff);
  return;
#else
  const unsigned int mapped = gwenesis_bus_map_address(address);
  md_bus_probe_record(mapped, 1, 16);

  switch (mapped) {

  case VDP_ADDR:
    md_bus_probe_vdp_port(address, 1);
    gwenesis_vdp_write_memory_16(address, value);
    return;

  case RAM_ADDR:
    WRITE16RAM(address, value);
    return;

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    ZRAM[address & 0X1FFF]= value >> 8;
    return;

  case IO_CTRL:
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    z80_write_ctrl(address & 0xFFFF, value >> 8) ;
    return;

  case Z80_YM2612_ADDR:
    bus_log(__FUNCTION__,"CZYM16 ,mclk=%d",  m68k_cycles_master());
    YM2612Write(address & 0x3, value >> 8,m68k_cycles_master() );
    return;

  case Z80_SN76489_ADDR:
    bus_log(__FUNCTION__,"CZSN16 ,mclk=%d", m68k_cycles_master());
    gwenesis_SN76489_Write(value >> 8,m68k_cycles_master() );
    return;

  default:
    md_bus_probe_fallback_write16();
    bus_log(__FUNCTION__,"write mem 16 default %x ", address);
    gwenesis_bus_write_memory_8(address, (value >> 8) & 0xff);
    gwenesis_bus_write_memory_8(address + 1, (value)&0xff);

    return;
  }
  return;
#endif
}

/******************************************************************************
 *
 *   68K CPU read address R8
 *   Read an address from memory mapped and return value as byte
 *
 ******************************************************************************/
unsigned int m68k_read_memory_8(unsigned int address)
{
#if MD_PUBLIC_RAM_FASTPATH
    if ((address & 0xE00000u) == 0xE00000u) return FETCH8RAM(address);
#endif
    return gwenesis_bus_read_memory_8(address);
}

/******************************************************************************
 *
 *   68K CPU read address R16
 *   Read an address from memory mapped and return value as word
 *
 ******************************************************************************/
 unsigned int m68k_read_memory_16(unsigned int address)
{
#if MD_PUBLIC_RAM_FASTPATH
    if ((address & 0xE00000u) == 0xE00000u) return FETCH16RAM(address);
#endif
#if MD_VDP_STATUS_READ_FASTPATH
    if (((address >> 16) & 0xFFu) == 0xC0u) {
      const unsigned int port = address & 0x0Eu;
      if (port == 0x04u || port == 0x06u) {
        md_bus_probe_record(VDP_ADDR, 0, 16);
        md_bus_probe_vdp_port(address, 0);
        return gwenesis_vdp_read_memory_16(address);
      }
    }
#endif
    return gwenesis_bus_read_memory_16(address);
}

/******************************************************************************
 *
 *   68K CPU read address R32
 *   Read an address from memory mapped and return value as long
 *
 ******************************************************************************/
 unsigned int m68k_read_memory_32(unsigned int address)
{
#if MD_PUBLIC_RAM_FASTPATH
    if ((address & 0xE00000u) == 0xE00000u) return FETCH32RAM(address);
#endif
    return (gwenesis_bus_read_memory_16(address) << 16) | gwenesis_bus_read_memory_16(address + 2);
}

/******************************************************************************
 *
 *   68K CPU write address W8
 *   Write an value as byte to memory mapped on specified address
 *
 ******************************************************************************/
void m68k_write_memory_8(unsigned int address, unsigned int value) {
#if MD_PUBLIC_RAM_FASTPATH
  if ((address & 0xE00000u) == 0xE00000u) {
    WRITE8RAM(address, value);
    return;
  }
#endif
  gwenesis_bus_write_memory_8(address, value);
  return;
}

/******************************************************************************
 *
 *   68K CPU write address W16
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void m68k_write_memory_16(unsigned int address, unsigned int value) {
#if MD_PUBLIC_RAM_FASTPATH
  if ((address & 0xE00000u) == 0xE00000u) {
    WRITE16RAM(address, value);
    return;
  }
#endif
  gwenesis_bus_write_memory_16(address, value);
  return;
}
/******************************************************************************
 *
 *   68K CPU write address W32
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void m68k_write_memory_32(unsigned int address, unsigned int value) {

#if MD_PUBLIC_RAM_FASTPATH
  if ((address & 0xE00000u) == 0xE00000u) {
    WRITE32RAM(address, value);
    return;
  }
#endif
  gwenesis_bus_write_memory_16(address, (value >> 16) & 0xffff);
  gwenesis_bus_write_memory_16(address + 2, (value)&0xffff);

  return;
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
    return m68k_read_memory_16(address);
}
unsigned int m68k_read_disassembler_32(unsigned int address)
{
    return m68k_read_memory_32(address);
}

void gwenesis_bus_save_state() {
  SaveState* state;
  state = saveGwenesisStateOpenForWrite("bus");
  saveGwenesisStateSetBuffer(state, "M68K_RAM", M68K_RAM, MAX_RAM_SIZE);
  saveGwenesisStateSetBuffer(state, "ZRAM", ZRAM, MAX_Z80_RAM_SIZE);
  saveGwenesisStateSetBuffer(state, "TMSS", TMSS, sizeof(TMSS));
  saveGwenesisStateSet(state, "tmss_state", tmss_state);
  saveGwenesisStateSet(state, "tmss_count", tmss_count);
}

void gwenesis_bus_load_state() {
    SaveState* state = saveGwenesisStateOpenForRead("bus");
    saveGwenesisStateGetBuffer(state, "M68K_RAM", M68K_RAM, MAX_RAM_SIZE);
    saveGwenesisStateGetBuffer(state, "ZRAM", ZRAM, MAX_Z80_RAM_SIZE);
    saveGwenesisStateGetBuffer(state, "TMSS", TMSS, sizeof(TMSS));
    tmss_state = saveGwenesisStateGet(state, "tmss_state");
    tmss_count = saveGwenesisStateGet(state, "tmss_count");
}

void gwenesis_init_sram(uint8_t *rom, uint32_t rom_size) {
  SRAM_ENABLED = 0;
  SRAM_START = 0;
  SRAM_END = 0;
  SRAM_SIZE = 0;

  if (SRAM != NULL) {
    free(SRAM);
    SRAM = NULL;
  }

  if (rom == NULL || rom_size < 0x1BC) {
    return;
  }

  if (FETCH8ROM(0x1B0) != 'R' || FETCH8ROM(0x1B1) != 'A') {
    return;
  }

  uint32_t start = ((uint32_t)FETCH8ROM(0x1B4) << 24) |
                   ((uint32_t)FETCH8ROM(0x1B5) << 16) |
                   ((uint32_t)FETCH8ROM(0x1B6) <<  8) |
                   ((uint32_t)FETCH8ROM(0x1B7));
  uint32_t end   = ((uint32_t)FETCH8ROM(0x1B8) << 24) |
                   ((uint32_t)FETCH8ROM(0x1B9) << 16) |
                   ((uint32_t)FETCH8ROM(0x1BA) <<  8) |
                   ((uint32_t)FETCH8ROM(0x1BB));

  if (end < start) {
    return;
  }

  uint32_t size = (end - start) + 1;

  if (size == 0 || size > 0x4000) {
    return;
  }

  SRAM = (uint8_t*)calloc(1, size);
  if (SRAM == NULL) {
#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
    EMU_LOG("SRAM alloc failed: %u\n", (unsigned)size);
#endif
    return;
  }

  SRAM_START = start;
  SRAM_END = end;
  SRAM_SIZE = size;
  SRAM_ENABLED = 1;

#if EMU_LOG_MASTER_ENABLED && defined(MD_LOGS)
  EMU_LOG("SRAM detected: start=%08X end=%08X size=%u\n",
          (unsigned)start, (unsigned)end, (unsigned)size);
#endif
}
