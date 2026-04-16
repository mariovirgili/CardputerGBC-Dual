#pragma once

struct MsxDisplayFrame;
class TFT_eSPI;

void msx_video_init(void);
void msx_video_shutdown(void);
void msx_video_lock(void);
void msx_video_unlock(void);
bool msx_video_present_frame(const MsxDisplayFrame* frame);
void msx_video_prepare_external_ui(void);
void msx_video_finish_external_ui(void);
TFT_eSPI& msx_video_external_tft(void);
void msx_video_set_runtime_menu_active(bool active);
void msx_video_set_state_overlay_active(bool active);
void msx_video_prepare_sd_access(void);
void msx_video_request_full_redraw(void);
