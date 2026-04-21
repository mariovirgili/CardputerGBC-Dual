#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../msx_config.h"

struct MsxDisplayFrame;
struct MsxVdpState;

using MsxVdpWriteVramFn = void (*)(MsxVdpState* state, uint32_t address, uint8_t value);

enum class MsxVdpMode : uint8_t {
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

enum class MsxVdpTransferCommand : uint8_t {
    None = 0,
    Lmcm,
    Lmmc,
    Hmmc,
    Hmmv,
    Lmmv,
    Lmmm,
    Hmmm,
    Ymmm,
};

struct MsxVdpCommandState {
    MsxVdpTransferCommand transfer;
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
    uint32_t cycleStamp;
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
    uint16_t screen8Palette565[256];
    uint32_t address;
    uint8_t latchedControl;
    uint8_t paletteLatch;
    MsxVdpWriteVramFn writeVram;
    bool palettePending;
    bool controlPending;
    bool dirty;
    bool frameReady;
    uint32_t frameCounter;
    uint32_t frameStartCpuCycles;
    uint32_t currentFrameCpuCycles;
    uint32_t frameCycleBudget;
    uint32_t lineInterruptFrameTag;
    uint8_t lineInterruptLineTag;
    unsigned activeWidth;
    unsigned activeHeight;
    MsxVdpMode mode;
    MsxVdpCommandState command;
};

bool msx_vdp_init(MsxVdpState* state, MsxMachineMode machineMode);
void msx_vdp_shutdown(MsxVdpState* state);
void msx_vdp_reset(MsxVdpState* state);
bool msx_vdp_begin_frame(MsxVdpState* state);
void msx_vdp_prepare_frame_render(MsxVdpState* state);
void msx_vdp_render(MsxVdpState* state);
void msx_vdp_render_slice(MsxVdpState* state, unsigned yStart, unsigned yEnd, bool finalizeFrame);
void msx_vdp_render_bitmap4_slice(MsxVdpState* state, unsigned yStart, unsigned yEnd, bool finalizeFrame);
void msx_vdp_advance_command_engine(MsxVdpState* state, uint32_t targetFrameCycles);
uint8_t msx_vdp_in_data(MsxVdpState* state);
uint8_t msx_vdp_in_status(MsxVdpState* state);
void msx_vdp_out_data(MsxVdpState* state, uint8_t value);
void msx_vdp_out_control(MsxVdpState* state, uint8_t value);
void msx_vdp_out_palette(MsxVdpState* state, uint8_t value);
void msx_vdp_out_indirect(MsxVdpState* state, uint8_t value);
void msx_vdp_get_display_frame(const MsxVdpState* state, MsxDisplayFrame* frame);
const char* msx_vdp_mode_label(MsxVdpMode mode);
bool msx_vdp_display_enabled(const MsxVdpState* state);


