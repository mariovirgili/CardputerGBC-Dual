#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../msx_config.h"

struct MsxDisplayFrame;

enum class MsxVdpMode : uint8_t {
    Graphics1 = 0,
    Text40,
    Graphics2,
    Multicolor,
    Graphics3,
    Bitmap4,
    Unsupported,
};

struct MsxVdpState {
    MsxMachineMode machineMode;
    uint8_t* vram;
    size_t vramSize;
    uint32_t vramMask;
    uint8_t* frameBuffer;
    bool ownsVram;
    uint8_t regs[64];
    uint8_t status[10];
    uint8_t readBuffer;
    uint8_t paletteRaw[16][2];
    uint16_t palette565[16];
    uint32_t address;
    uint8_t latchedControl;
    uint8_t paletteLatch;
    bool palettePending;
    bool controlPending;
    bool dirty;
    bool frameReady;
    uint32_t frameCounter;
    unsigned activeWidth;
    unsigned activeHeight;
    MsxVdpMode mode;
};

bool msx_vdp_init(MsxVdpState* state, MsxMachineMode machineMode);
void msx_vdp_shutdown(MsxVdpState* state);
void msx_vdp_reset(MsxVdpState* state);
bool msx_vdp_begin_frame(MsxVdpState* state);
void msx_vdp_render(MsxVdpState* state);
uint8_t msx_vdp_in_data(MsxVdpState* state);
uint8_t msx_vdp_in_status(MsxVdpState* state);
void msx_vdp_out_data(MsxVdpState* state, uint8_t value);
void msx_vdp_out_control(MsxVdpState* state, uint8_t value);
void msx_vdp_out_palette(MsxVdpState* state, uint8_t value);
void msx_vdp_out_indirect(MsxVdpState* state, uint8_t value);
void msx_vdp_get_display_frame(const MsxVdpState* state, MsxDisplayFrame* frame);
const char* msx_vdp_mode_label(MsxVdpMode mode);
bool msx_vdp_display_enabled(const MsxVdpState* state);


