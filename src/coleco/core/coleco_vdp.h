#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../coleco_config.h"

struct ColecoDisplayFrame;

enum class ColecoVdpMode : uint8_t {
    Graphics1 = 0,
    Text40,
    Graphics2,
    Multicolor,
    Graphics3,
    Bitmap4,
    Bitmap6,
    Bitmap7,
    Bitmap8,
    Text80,
    Unsupported,
};

enum class ColecoVdpTransferCommand : uint8_t {
    None = 0,
    Lmcm,
    Lmmc,
    Hmmc,
};

struct ColecoVdpCommandState {
    ColecoVdpTransferCommand transfer;
    uint8_t screenMode;
    uint8_t logicOp;
    uint16_t sx;
    uint16_t sy;
    uint16_t dx;
    uint16_t dy;
    uint16_t nx;
    uint16_t ny;
    uint16_t asx;
    uint16_t adx;
    uint16_t anx;
    uint16_t mx;
    int16_t tx;
    int16_t ty;
};

struct ColecoVdpState {
    ColecoMachineMode machineMode;
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
    uint16_t screen8Palette565[256];
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
    ColecoVdpMode mode;
    ColecoVdpCommandState command;
};

bool coleco_vdp_init(ColecoVdpState* state, ColecoMachineMode machineMode);
void coleco_vdp_shutdown(ColecoVdpState* state);
void coleco_vdp_reset(ColecoVdpState* state);
bool coleco_vdp_begin_frame(ColecoVdpState* state);
void coleco_vdp_render(ColecoVdpState* state);
uint8_t coleco_vdp_in_data(ColecoVdpState* state);
uint8_t coleco_vdp_in_status(ColecoVdpState* state);
void coleco_vdp_out_data(ColecoVdpState* state, uint8_t value);
void coleco_vdp_out_control(ColecoVdpState* state, uint8_t value);
void coleco_vdp_out_palette(ColecoVdpState* state, uint8_t value);
void coleco_vdp_out_indirect(ColecoVdpState* state, uint8_t value);
void coleco_vdp_get_display_frame(const ColecoVdpState* state, ColecoDisplayFrame* frame);
const char* coleco_vdp_mode_label(ColecoVdpMode mode);
bool coleco_vdp_display_enabled(const ColecoVdpState* state);


