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
bool msx_video_prepare_msx2_stream_buffers(unsigned srcW, unsigned srcH);
void msx_video_prepare_external_ui(void);
void msx_video_finish_external_ui(void);
TFT_eSPI& msx_video_external_tft(void);
void msx_video_set_runtime_menu_active(bool active);
void msx_video_set_state_overlay_active(bool active);
void msx_video_prepare_sd_access(void);
void msx_video_request_full_redraw(void);
bool msx_video_scroll_internal_zoom(int dx, int dy);
void msx_video_center_internal_zoom(void);
bool msx_video_internal_zoom_active(void);
void msx_video_set_zoom_follow_enabled(bool enabled);
bool msx_video_get_zoom_follow_enabled(void);
void msx_video_set_zoom_follow_input(bool left, bool right, bool up, bool down);
MsxVideoPerfSummary msx_video_get_perf_summary(void);
uint32_t msx_video_get_last_present_us(void);
void msx_video_clear_last_present_us(void);
void msx_video_set_fps_overlay_value(uint16_t fps10);
void msx_video_set_fps_overlay_values(uint16_t coreFps10, uint16_t displayFps10);
uint32_t msx_video_get_presented_frame_counter(void);
