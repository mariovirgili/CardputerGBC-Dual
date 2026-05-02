#pragma once

#include <stddef.h>
#include <stdint.h>

#include "msx_bios.h"
#include "msx_cart.h"
#include "msx_cpu.h"
#include "msx_disk.h"
#include "msx_memory.h"
#include "msx_psg.h"
#include "msx_scc.h"
#include "msx_vdp.h"
#include "../msx_display.h"
#include "../msx_input.h"
#include "../msx_media.h"

struct MsxCoreState {
    bool initialized;
    bool directBoot;
    bool videoHookReady;
    bool audioHookReady;
    uint32_t frameCounter;
    uint32_t lastFrameCpuUs;
    uint32_t lastFrameVdpUs;
    uint32_t lastFramePresentUs;
    uint32_t lastFrameOtherUs;
    uint32_t lastFrameTotalUs;
    uint32_t lastFrameCycles;
    uint32_t lastStatusFrame;
    uint32_t audioSampleRate;
    uint16_t lastAudioSamples;
    MsxRomImage rom;
    MsxBiosTarget biosTarget;
    MsxRegionProfile regionProfile;
    MsxMachineMode machineMode;
    uint16_t bootPc;
    uint16_t lastStatusPc;
    uint8_t lastStatusMode;
    uint8_t lastStatusRunState;
    uint8_t lastStatusUnsupportedOpcode;
    uint8_t lastInputJoy;
    uint8_t lastInputConfigFlags;
    bool lastInputJoystickMode;
    bool lastInputCaptured;
    char statusText[64];
    char romName[96];
    MsxDisplayFrame displayFrame;
    MsxKeyboardMatrix lastInputKeyboardMatrix;
    MsxBiosState bios;
    MsxCartState cart;
    MsxDiskState disk;
    MsxCasState cas;
    MsxMemoryState memory;
    MsxVdpState vdp;
    MsxPsgState psg;
    MsxSccState scc;
    MsxCpuState cpu;
};

bool msx_core_init(MsxCoreState* state,
                   const MsxRomImage* rom,
                   const MsxBiosBundle* bios,
                   const char* romName,
                   uint32_t audioSampleRate,
                   const uint8_t* cartSramData = nullptr,
                   size_t cartSramSize = 0u);

bool msx_core_init_basic(MsxCoreState* state,
                         const MsxBiosBundle* bios,
                         const char* name,
                         uint32_t audioSampleRate);

bool msx_core_init_disk(MsxCoreState* state,
                        const MsxBiosBundle* bios,
                        const uint8_t* diskRomData, size_t diskRomSize,
                        const uint8_t* dskData, size_t dskSize,
                        const char* name,
                        uint32_t audioSampleRate);

// Launch MSX BASIC with a CAS cassette tape image attached.
// casData/casSize point to the raw CAS file (XIP-mapped or in RAM).
bool msx_core_init_cas(MsxCoreState* state,
                       const MsxBiosBundle* bios,
                       const uint8_t* casData, size_t casSize,
                       const char* name,
                       uint32_t audioSampleRate);

bool msx_core_change_cas(MsxCoreState* state,
                         const uint8_t* casData, size_t casSize,
                         const char* name);

bool msx_core_change_dsk(MsxCoreState* state,
                         const uint8_t* dskData, size_t dskSize,
                         const char* name);

void msx_core_attach_disk_rom(MsxCoreState* state,
                              const uint8_t* diskRomData,
                              size_t diskRomSize);

void msx_core_handle_input(MsxCoreState* state, const MsxInputState* input);
void msx_core_set_virtual_scc_mode(MsxCoreState* state, MsxVirtualSccMode mode);
void msx_core_set_scc_hardware_detect(MsxCoreState* state, bool enabled);
void msx_core_set_region_profile(MsxCoreState* state, MsxRegionProfile profile);
void msx_core_step_frame(MsxCoreState* state);
size_t msx_core_drain_audio(MsxCoreState* state, int16_t* dst, size_t capacity);
void msx_core_shutdown(MsxCoreState* state);
