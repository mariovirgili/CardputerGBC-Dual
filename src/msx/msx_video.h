#pragma once

struct MsxDisplayFrame;

void msx_video_init(void);
void msx_video_shutdown(void);
bool msx_video_present_frame(const MsxDisplayFrame* frame);
