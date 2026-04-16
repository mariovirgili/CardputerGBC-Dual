#pragma once

struct ColecoDisplayFrame;
class TFT_eSPI;

void coleco_video_init(void);
void coleco_video_shutdown(void);
void coleco_video_lock(void);
void coleco_video_unlock(void);
bool coleco_video_present_frame(const ColecoDisplayFrame* frame);
void coleco_video_prepare_external_ui(void);
void coleco_video_finish_external_ui(void);
TFT_eSPI& coleco_video_external_tft(void);
void coleco_video_set_runtime_menu_active(bool active);
void coleco_video_prepare_sd_access(void);
void coleco_video_request_full_redraw(void);
