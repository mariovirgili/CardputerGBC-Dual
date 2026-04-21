#pragma once

#include <cstdint>

struct MsxDisplayFrame;
class TFT_eSPI;

struct MsxVideoPerfSummary {
    bool valid;
    uint32_t windowFrames;
    uint32_t pushedFrames;
    uint32_t avgPresentUs;
    uint32_t worstPresentUs;
    uint32_t overBudgetFrames;
    uint32_t overHalfRateFrames;
    uint32_t presentFails;
    uint32_t skippedFrames;
    uint16_t frameskipPercent;
};

void msx_video_init(void);
void msx_video_shutdown(void);
void msx_video_lock(void);
void msx_video_unlock(void);
bool msx_video_present_frame(const MsxDisplayFrame* frame);
bool msx_video_begin_line_stream(const MsxDisplayFrame* frame);
bool msx_video_stream_line(const MsxDisplayFrame* frame, const uint8_t* srcLine, unsigned srcLineIndex);
void msx_video_end_line_stream(void);
void msx_video_prepare_external_ui(void);
void msx_video_finish_external_ui(void);
TFT_eSPI& msx_video_external_tft(void);
void msx_video_set_runtime_menu_active(bool active);
void msx_video_set_state_overlay_active(bool active);
void msx_video_prepare_sd_access(void);
void msx_video_request_full_redraw(void);
MsxVideoPerfSummary msx_video_get_perf_summary(void);
