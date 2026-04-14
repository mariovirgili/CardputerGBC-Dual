#include "run_coleco.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <SD.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "cardputer/CardputerInput.h"
#include "cardputer/CardputerView.h"
#include "select_coleco_bios.h"
#include "esp_timer.h"
#include "coleco_config.h"
#include "coleco_display.h"
#include "coleco_input.h"
#include "coleco_media.h"
#include "coleco_video.h"
#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include "coleco_sound.h"
#include "share/display_target.h"
#include "share/emu_controls.h"
#include "share/game_save.h"
#include "share/utils.h"

#include "core/coleco_cpu.h"
#include "core/coleco_memory.h"
#include "core/coleco_vdp.h"
#include "core/SN76489.h"

struct PsgChannel {
    int freq;
    int vol;
    float phase;
    uint16_t lfsr;
};
static PsgChannel g_psg_ch[4];

static void psg_sound_callback(int C, int F, int V) {
    // We handle PSG natively in run_coleco via coleco_sound_submit
    if (C >= 0 && C < 4) {
        g_psg_ch[C].freq = F;
        g_psg_ch[C].vol = V;
    }
}

void run_coleco(const uint8_t* romData, size_t romLen, const char* romName, SdService& sd)
{
    printf("[COLECO] run_coleco start, ROM size=%zu, free heap=%lu\n",
        romLen, (unsigned long)esp_get_free_heap_size());

    // Pre-allochiamo anche il buffer del BIOS (8KB) per evitare frammentazioni
    ColecoBiosImage bios;
    memset(&bios, 0, sizeof(ColecoBiosImage));
    bios.data = (uint8_t*)heap_caps_calloc(1, 8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // Pre-allocate VDP buffers (87KB total) as the very first malloc so the
    // heap is still contiguous. BIOS and sound allocations happen afterwards.
    ColecoVdpState vdp;
    memset(&vdp, 0, sizeof(ColecoVdpState));

    bool vdp_ok = coleco_vdp_init(&vdp, ColecoMachineMode::MSX1);

    if (!vdp_ok) {
        CardputerView display;
        display.initialize();
        display.topBar("ERROR", false, false);
        display.subMessage("VDP init failed (OOM)", 2000);
        return;
    }
    if (!vdp.frameBuffer) {
        printf("[COLECO] WARNING: frameBuffer NULL after VDP init - rendering disabled\n");
    }

    // Resolve BIOS path: use saved config, browse if missing/invalid
    coleco_config_load_bios_path();
    const char* savedBiosPath = coleco_config_get_bios_path();

    std::string biosPath;
    if (savedBiosPath && savedBiosPath[0] != '\0') {
        biosPath = savedBiosPath;
        printf("[COLECO] Saved BIOS path: %s\n", biosPath.c_str());
    }

    // Try to open the saved path; if it fails, launch the browser
    {
        const char* rawPath = biosPath.c_str();
        const char* sdPath = (strncmp(rawPath, "/sd/", 4) == 0) ? rawPath + 3 : rawPath;
        File f = biosPath.empty() ? File() : SD.open(sdPath);
        bool valid = f && !f.isDirectory();
        if (f) f.close();
        if (!valid) biosPath = "";
    }
    if (biosPath.empty()) {
        printf("[COLECO] BIOS not found at saved path, launching browser\n");
        CardputerView biosDisplay;
        CardputerInput biosInput;
        biosPath = selectColecoBiosPath(sd, biosDisplay, biosInput);

        if (biosPath.empty()) {
            coleco_vdp_shutdown(&vdp);
            coleco_media_release_bios(&bios);
            CardputerView display;
            display.initialize();
            display.topBar("ERROR", false, false);
            display.subMessage("No BIOS selected", 2000);
            return;
        }

        coleco_config_set_bios_path(biosPath.c_str(), true);
        printf("[COLECO] BIOS path saved: %s\n", biosPath.c_str());
    }

    if (!coleco_media_load_bios(&bios, biosPath.c_str())) {
        coleco_config_set_bios_path("", true); // clear invalid path
        coleco_vdp_shutdown(&vdp);
        coleco_media_release_bios(&bios);
        CardputerView display;
        display.initialize();
        display.topBar("ERROR", false, false);
        display.subMessage("BIOS load failed", 2000);
        return;
    }

    const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);

    {
        CardputerView display;
        display.initialize();
        
        if (useExternal) {
            display.topBar("COLECO EXT TFT", false, false);
        } else {
            display.topBar("COLECO INTERNAL", false, false);
            if (!emu_is_aux_screen_locked()) {
                extern void coleco_display_show_external_info(const char* romTitle);
                coleco_display_show_external_info(romName);
            }
        }
        
        display.showControlBindings(
            share::emuControlActionLabels(share::EmuProfile::MSX),
            share::emuControlKeyLabels(share::EmuProfile::MSX),
            "GO = QUIT  HOLD GO = MENU"
        );

        M5Cardputer.Display.setTextFont(1);
        M5Cardputer.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
        M5Cardputer.Display.setTextDatum(top_center);
        M5Cardputer.Display.drawString("ColecoVision: .col", M5Cardputer.Display.width() / 2, 20);

        vTaskDelay(pdMS_TO_TICKS(2000)); // Pausa di 2 secondi per permettere la lettura dei comandi
    }

    coleco_config_load_internal_view_mode();

    coleco_display_init();
    coleco_input_init();

    // Init sound
    uint32_t sampleRate = 44100;
    coleco_sound_init(sampleRate, 1);

    ColecoMemoryState memory;
    ColecoCpuState cpu;
    SN76489 psg;

    coleco_memory_init(&memory, bios.data, bios.size, romData, romLen);
    coleco_memory_attach_vdp(&memory, &vdp);
    
    memset(g_psg_ch, 0, sizeof(g_psg_ch));
    Reset76489(&psg, psg_sound_callback);
    coleco_memory_attach_psg(&memory, &psg);

    coleco_cpu_init(&cpu);
    coleco_cpu_reset(&cpu, 0x0000, 0x0000);

    printf("[COLECO] Init done, free heap=%lu\n",
        (unsigned long)esp_get_free_heap_size());

    bool quitRequested = false;
    uint32_t lastFrameUs = (uint32_t)esp_timer_get_time();
    const uint32_t frameTimeUs = 16667; // 60 Hz

    int cycleBudget = 0;
    const int CYCLES_PER_FRAME = 3579545 / 60;

    uint32_t fpsFrameCount = 0;
    uint32_t fpsLastUs = (uint32_t)esp_timer_get_time();

    // Identifichiamo il sistema in esecuzione: il BIOS Coleco pesa esattamente 8KB
    const bool isColeco = (bios.size == 8192);

    while (!quitRequested) {
        uint32_t nowUs = (uint32_t)esp_timer_get_time();
        if (nowUs - lastFrameUs < frameTimeUs) {
            vTaskDelay(1);
            continue;
        }
        lastFrameUs = nowUs;

        ColecoInputState inputState;
        coleco_input_poll(&inputState);
        
        uint16_t joy_high = 0xFF; // All released (active low)
        if (inputState.up) joy_high &= ~0x01;
        if (inputState.right) joy_high &= ~0x02;
        if (inputState.down) joy_high &= ~0x04;
        if (inputState.left) joy_high &= ~0x08;
        if (inputState.fire1) joy_high &= ~0x40;

        uint16_t joy_low = 0xFF; // Keypad None (0x0F) and Fire2 released
        if (M5Cardputer.Keyboard.isKeyPressed('1')) joy_low = (joy_low & ~0x0F) | 1;
        else if (M5Cardputer.Keyboard.isKeyPressed('2')) joy_low = (joy_low & ~0x0F) | 2;
        else if (M5Cardputer.Keyboard.isKeyPressed('3')) joy_low = (joy_low & ~0x0F) | 3;
        else if (M5Cardputer.Keyboard.isKeyPressed('4')) joy_low = (joy_low & ~0x0F) | 4;
        else if (M5Cardputer.Keyboard.isKeyPressed('5')) joy_low = (joy_low & ~0x0F) | 5;
        else if (M5Cardputer.Keyboard.isKeyPressed('6')) joy_low = (joy_low & ~0x0F) | 6;
        else if (M5Cardputer.Keyboard.isKeyPressed('7')) joy_low = (joy_low & ~0x0F) | 7;
        else if (M5Cardputer.Keyboard.isKeyPressed('8')) joy_low = (joy_low & ~0x0F) | 8;
        else if (M5Cardputer.Keyboard.isKeyPressed('9')) joy_low = (joy_low & ~0x0F) | 9;
        else if (M5Cardputer.Keyboard.isKeyPressed('0')) joy_low = (joy_low & ~0x0F) | 10;
        else if (M5Cardputer.Keyboard.isKeyPressed('-')) joy_low = (joy_low & ~0x0F) | 11;
        else if (M5Cardputer.Keyboard.isKeyPressed('=')) joy_low = (joy_low & ~0x0F) | 12;

        if (inputState.fire2) joy_low &= ~0x40;

        memory.joyState[0] = (joy_high << 8) | joy_low;

        if (inputState.quitRequested) {
            quitRequested = true;
            break;
        }

        if (inputState.toggleViewRequested) {
            // handle zoom
            if (!useExternal) {
                coleco_config_toggle_internal_view_mode();
                coleco_display_init(); // re-init display parameters
            }
        }

        cycleBudget += CYCLES_PER_FRAME;
        
        while (cycleBudget > 0) {
            int executed = coleco_cpu_run_cycles(&cpu, &memory, 100);
            cycleBudget -= executed;
            
            if (cycleBudget <= 0) {
                if (isColeco) {
                    cpu.nmiPending = true; // I giochi ColecoVision attendono il VBLANK via NMI
                } else {
                    cpu.irqPending = true; // I giochi MSX usano il normale IRQ
                }
            }
        }

        fpsFrameCount++;
        if (fpsFrameCount >= 60) {
            uint32_t nowForFps = (uint32_t)esp_timer_get_time();
            uint32_t elapsedUs = nowForFps - fpsLastUs;
            float fps = elapsedUs > 0 ? (fpsFrameCount * 1000000.0f / elapsedUs) : 0.0f;
            printf("[COLECO] FPS=%.1f  heap=%lu\n",
                fps, (unsigned long)esp_get_free_heap_size());
            fpsFrameCount = 0;
            fpsLastUs = nowForFps;
        }

        // Render VDP
        if (coleco_vdp_begin_frame(&vdp)) {
            coleco_vdp_render(&vdp);
            ColecoDisplayFrame displayFrame;
            coleco_vdp_get_display_frame(&vdp, &displayFrame);
            
            ColecoDisplayStatus status;
            status.romName = romName;
            status.coreLine = "ColecoVision";
            status.cartLine = "";
            status.machineLine = "TMS9918";
            status.biosLine = "coleco.rom";
            status.audioLine = "SN76489";
            status.frameCounter = vdp.frameCounter;
            
            coleco_display_submit_frame(&displayFrame, &status);
        }
        
        // Sync Audio
        Sync76489(&psg, 1);
        size_t capacity = 0;
        int16_t* buf = coleco_sound_begin_mix(&capacity);
        if (buf && capacity > 0) {
            for(size_t i = 0; i < capacity; i++) {
                int32_t mix = 0;

                for (int c = 0; c < 3; c++) {
                    if (g_psg_ch[c].vol > 0 && g_psg_ch[c].freq > 0) {
                        float freqHz = g_psg_ch[c].freq;
                        float phaseInc = freqHz / 44100.0f;
                        
                        g_psg_ch[c].phase += phaseInc;
                        while (g_psg_ch[c].phase >= 1.0f) g_psg_ch[c].phase -= 1.0f;
                        
                        int amplitude = g_psg_ch[c].vol * 30;
                        if (g_psg_ch[c].phase < 0.5f) mix += amplitude;
                        else mix -= amplitude;
                    }
                }
                
                // Noise channel (c = 3)
                if (g_psg_ch[3].vol > 0 && g_psg_ch[3].freq > 0) {
                    float freqHz = g_psg_ch[3].freq;
                    float phaseInc = freqHz / 44100.0f;
                    
                    g_psg_ch[3].phase += phaseInc;
                    while (g_psg_ch[3].phase >= 1.0f) {
                        g_psg_ch[3].phase -= 1.0f;
                        uint16_t lfsr = g_psg_ch[3].lfsr;
                        if (lfsr == 0) lfsr = 0x8000;
                        int bit = (lfsr & 1) ^ ((lfsr >> 3) & 1); // White noise
                        g_psg_ch[3].lfsr = (lfsr >> 1) | (bit << 14);
                    }
                    
                    int amplitude = g_psg_ch[3].vol * 30;
                    if (g_psg_ch[3].lfsr & 1) mix += amplitude;
                    else mix -= amplitude;
                }
                
                if (mix > 32767) mix = 32767;
                if (mix < -32768) mix = -32768;
                buf[i] = mix;
            }
            coleco_sound_end_mix(capacity);
        }
    }

    coleco_sound_shutdown();
    coleco_vdp_shutdown(&vdp);
    coleco_memory_shutdown(&memory);
    coleco_media_release_bios(&bios);
    coleco_display_shutdown();
}
