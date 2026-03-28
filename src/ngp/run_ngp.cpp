#include <Arduino.h> 
#include <string.h>
#include <M5Cardputer.h>
#include "run_ngp.h"
#include "../race/race-memory.h"
#include "../race/types.h"
#include "../race/tlcs900h.h"
#include "../race/input.h"
#include "../race/flash.h"
#include "ngc_sound.h"
#include "ngc_input.h"
#include "ngc_display.h"
#include "ngc_scheduler.h"
#include "esp_timer.h" // used for get_time()
#include "../../share/utils.h"
#include "esp_rom_sys.h" // used for esp_rom_delay_us(us)
#include "../share/display_target.h"
#include "../share/emu_controls.h"
#include "../cardputer/CardputerView.h"
// #include "ngc_bios.h"

#define NGP_LANG_EN 1
#define NGP_LANG    NGP_LANG_EN  // 0 = JP, 1 = EN
char retro_save_directory[3] = "/";
int tipo_consola = 0;

extern "C" {

int m_bIsActive = 1;
int gfx_hacks = 0;

extern int finscan;
extern "C" unsigned int gen_regsPC;
#define tlcsPc gen_regsPC
extern EMUINFO m_emuInfo;
extern int tipo_consola;
__attribute__((weak)) void tlcs_reset(void) {}

unsigned char *rasterY = 0;
unsigned char  *frame0Pri = 0;
unsigned char  *frame1Pri = 0;
unsigned char  *color_switch = 0;
unsigned char *scanlineY = 0;
unsigned char *scrollSpriteX = 0;
unsigned char *scrollSpriteY = 0;
unsigned char *sprite_palette_numbers = 0;
unsigned char *sprite_table = 0;
unsigned short *patterns = 0;
unsigned char *oowSelect = 0;
unsigned short *oowTable = 0;
unsigned char *wndTopLeftY = 0;
unsigned char *wndSizeY = 0;
unsigned char *wndSizeX = 0;
unsigned char *bgSelect = 0;
unsigned char *bw_palette_table = 0;
unsigned short *palette_table = 0;
unsigned short *bgTable = 0;
unsigned char *wndTopLeftX = 0;
unsigned char *scrollFrontY = 0;
unsigned char *scrollFrontX = 0;
unsigned short *tile_table_front = 0;
unsigned char *scrollBackY = 0;
unsigned short *tile_table_back = 0;
unsigned char *scrollBackX = 0;
unsigned char  *pattern_table = NULL;

void ngpSoundOff(void);
void ngpSoundExecute(void) __attribute__((weak));
void ngpSoundExecute(void) {}
void audio_dac_init(void);
int Cz80_allocate_flag_tables(void);

}

static void set_defaults_after_boot(void)
{
  // Couleur/mono selon ROM
  tlcsMemWriteB(0x00006F91, tlcsMemReadB(0x00200023));
  if (tipo_consola == 1) tlcsMemWriteB(0x00006F91, 0x00); // forcer mono

  // Langue ROM
  tlcsMemWriteB(0x00006F87, (NGP_LANG == NGP_LANG_EN) ? 0x01 : 0x00);

  // IRQ video
  tlcsMemWriteB(0x00004000, tlcsMemReadB(0x00004000) | 0xC0);

  // Bits Power
  tlcsMemWriteB(0x00006F84, 0x40);
  tlcsMemWriteB(0x00006F85, 0x00);
  tlcsMemWriteB(0x00006F86, 0x00);
}

static void map_vdp_tables_full()
{
  sprite_table           = (unsigned char*)  get_address(0x00008800);
  pattern_table          = (unsigned char*)  get_address(0x0000A000);
  patterns               = (unsigned short*) pattern_table;
  tile_table_front       = (unsigned short*) get_address(0x00009000);
  tile_table_back        = (unsigned short*) get_address(0x00009800);
  palette_table          = (unsigned short*) get_address(0x00008200);
  bw_palette_table       = (unsigned char*)  get_address(0x00008100);
  sprite_palette_numbers = (unsigned char*)  get_address(0x00008C00);

  scanlineY              = (unsigned char*)  get_address(0x00008009);
  frame0Pri              = (unsigned char*)  get_address(0x00008000);
  frame1Pri              = (unsigned char*)  get_address(0x00008030);

  wndTopLeftX            = (unsigned char*)  get_address(0x00008002);
  wndTopLeftY            = (unsigned char*)  get_address(0x00008003);
  wndSizeX               = (unsigned char*)  get_address(0x00008004);
  wndSizeY               = (unsigned char*)  get_address(0x00008005);

  scrollSpriteX          = (unsigned char*)  get_address(0x00008020);
  scrollSpriteY          = (unsigned char*)  get_address(0x00008021);
  scrollFrontX           = (unsigned char*)  get_address(0x00008032);
  scrollFrontY           = (unsigned char*)  get_address(0x00008033);
  scrollBackX            = (unsigned char*)  get_address(0x00008034);
  scrollBackY            = (unsigned char*)  get_address(0x00008035);

  bgSelect               = (unsigned char*)  get_address(0x00008118);
  bgTable                = (unsigned short*) get_address(0x000083E0);
  oowSelect              = (unsigned char*)  get_address(0x00008012);
  oowTable               = (unsigned short*) get_address(0x000083F0);

  color_switch           = (unsigned char*)  get_address(0x00006F91);

  static unsigned char s_dummy_scan = 0;
  if (!scanlineY) scanlineY = &s_dummy_scan;
  rasterY = scanlineY;
}

void run_ngp(const uint8_t* rom_base, size_t rom_size, int machine, const char* romName)
{
  // Load ROM
  ngp_mem_set_rom(rom_base, rom_size);

  // Sys info
  m_emuInfo.machine = machine;
  m_emuInfo.romSize = (int)rom_size;
  tipo_consola      = 0;      // 0 = NGPC, 1 = NGP (mono)

  // Dual-screen info: show info on whichever screen is NOT rendering the game
  const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
  {
    CardputerView display;
    display.initialize();
    if (useExternal) {
      display.topBar("NGP ON EXTERNAL TFT", false, false);
      display.showControlBindings(
        share::emuControlActionLabels(share::EmuProfile::Ngp),
        share::emuControlKeyLabels(share::EmuProfile::Ngp),
        "GO / HOLD ESC = QUIT"
      );
    } else {
      if (!emu_is_aux_screen_locked()) {
        ngc_display_show_external_info(romName);
      }
    }
  }

  // Core init
  Cz80_allocate_flag_tables();
  ngp_mem_init();

  // Map VRAM/regs
  map_vdp_tables_full();

  // default BG palette + enable
  if (bgTable)   bgTable[0] = 0xFFFF;
  if (bgSelect) *bgSelect |= 0x80;  // enable bgTable[index]

  // CPU
  tlcs_init();
  tlcs_reset();
  Z80_Init();
  Z80_Reset();

  // Flags post boot
  set_defaults_after_boot();

  // Init video
  ngc_display_init();
  graphics_init();

  // Fullscreen si vide
  if (wndTopLeftX && wndTopLeftY && wndSizeX && wndSizeY) {
    if (*wndSizeX == 0 || *wndSizeY == 0) {
      *wndTopLeftX = 0; *wndTopLeftY = 0;
      *wndSizeX    = 160; *wndSizeY  = 152;
    }
  }
  if (bgSelect) *bgSelect |= 0x80;

  // LCD ON + priorité frame0
  if (frame0Pri) *frame0Pri |= 0x80 | 0x40;

  // Master IE (VBlank/HBlank)
  tlcsMemWriteB(0x00004000, tlcsMemReadB(0x00004000) | 0xC0);

  // Fin ecran selon rom
  finscan = 198;
  if (mainrom[0x000020] == 0x65 || mainrom[0x000020] == 0x93) finscan = 199;

  audio_dac_init();
  ngc_sound_init();
  ngc_sound_frame();
  ngc_sound_frame();
  ngc_input_init();
  ngc_scheduler_start();

  // Go
  printf("[NGPC_RUN] entering ngpc_run() ... m_bIsActive=%d\n", m_bIsActive);
  printf("[FORCE] Enabling LCD + VBLANK IRQ @ 0x4000 = 0xC0\n");
  tlcsMemWriteB(0x00004000, 0xC0);   // bit 7 = LCD ON, bit 6 = VBlank IRQ enable
  m_bIsActive = 1;

  printf("[NGPC_RUN] starting core loop\n");

  unsigned long now = esp_timer_get_time();
  unsigned long status_last = millis();
  unsigned long frames = 0;
  unsigned long frame_time_total = 0;
  unsigned long frame_time_min = ULONG_MAX;
  unsigned long frame_time_max = 0;
  const uint32_t TARGET_US = 16667; // 60 Hz
  const uint32_t CPU_CLOCK_HZ = 5700000; // 6 MHz downclocked by 5% (smooth perfs)
  unsigned long next_deadline = now + TARGET_US;
  float MAX_LAG_FRAMES = 1.1; // max number of frames we can lag, prevents overruns after a period of running slow
  uint32_t MAX_LAG_US = (MAX_LAG_FRAMES * TARGET_US);
#ifdef FRAMESKIP
  unsigned int frame_skipped = 0;
  unsigned int frames_to_render = 0; // how many frames to render before skipping is an possibility  
#endif


  // Kludges ROM
  switch (tlcsMemReadW(0x00200020)) {
    case 0x0059:   // Sonic
    case 0x0061:   // Metal Slug 2nd
        tlcsMemWriteB(0x0020001F, 0xFF);
        // ngc_patch_snk_logo();
      break;
  }

  while (m_bIsActive)
  {
      unsigned long t0 = esp_timer_get_time();
      unsigned long t0ms = micros();  

      // Execute one frame
      tlcs_execute((CPU_CLOCK_HZ) / 60);

      // Feed audio each frame
      ngc_sound_frame();

      // Log framerate
      uint32_t emuUs = micros() - t0ms;
      frame_time_total += emuUs;
      if (emuUs < frame_time_min) frame_time_min = emuUs;
      if (emuUs > frame_time_max) frame_time_max = emuUs;
      frames++;

      // Pacing 60 Hz
      now = esp_timer_get_time();

      // We fell way behind — drop backlog
      if (now > next_deadline + MAX_LAG_US)
        next_deadline = now - MAX_LAG_US; // clamp to ceiling.

      // If we're early, wait 
      if (now < next_deadline) {
          share::sleep_until_us(next_deadline-200); // throw away a little time to use for the next frame, 200us*60 = 12ms, not even one frame extra a second
          now = next_deadline;
      }
    
      #ifdef FRAMESKIP
      if ((now >= (next_deadline + TARGET_US)) && (frames_to_render <= 0))
      {
        frames_to_render = 4; // This forces a skip frame not to be conisdered again for 5 frames, the first is decremented during this triggered skip frame
        // Queue a skip frame to catch up
        tlcs_queueFrameSkip(1);
        frame_skipped++;
        
      }
      else
      { // Only advance time if we are not skipping a frame
        // next_deadline += TARGET_US;
        if (frames_to_render > 0)
          frames_to_render--;
      }
      #endif
      next_deadline += TARGET_US;
      

      if (millis() - status_last >= 2000)
      {
          size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
          float avg_ms = frame_time_total / (float)frames / 1000.0f;
          float min_ms = frame_time_min / 1000.0f;
          float max_ms = frame_time_max / 1000.0f;
          #ifdef FRAMESKIP
          printf("[NGP_RUN] %lu[%d] frames / 2s (~%lu FPS) | HEAP: %u bytes (%.1f KB) | AVG %.2fms | MIN %.2fms | MAX %.2fms\n",
          frames,
          frame_skipped,
          frames / 2,
          (unsigned int)heap_free,
          heap_free / 1024.0f,
          avg_ms,
          min_ms,
          max_ms);
          frame_skipped=0;
          #else
          printf("[NGP_RUN] %lu frames / 2s (~%lu FPS) | HEAP: %u bytes (%.1f KB) | AVG %.2fms | MIN %.2fms | MAX %.2fms\n",
          frames,
          frames / 2,
          (unsigned int)heap_free,
          heap_free / 1024.0f,
          avg_ms,
          min_ms,
          max_ms);
          #endif
          frame_time_total = 0;
          frame_time_min = ULONG_MAX;
          frame_time_max = 0;
          frames = 0;
          status_last = millis();
      }
  }
}
