#pragma once

struct MsxDisplayFrame;
class TFT_eSPI;

void msx_video_init(void);
void msx_video_shutdown(void);
bool msx_video_present_frame(const MsxDisplayFrame* frame);
void msx_video_prepare_external_ui(void);
void msx_video_finish_external_ui(void);
TFT_eSPI& msx_video_external_tft(void);
