#pragma once
#include <stdint.h>

extern bool fullscreen;
extern bool scanline;
extern int smsZoomPercent;

#ifdef __cplusplus
extern "C" {
  #include "sms/smsplus/shared.h"
  #include "sms/smsplus/vdp.h"
}
#endif

void sms_display_init();
void sms_palette_init_fixed();
void video_compute_scaler_full();
void video_compute_scaler_square();
void sms_display_write_frame();
void sms_display_clear();

#ifdef __cplusplus
// Show ROM info + controls on external TFT (when game renders on internal)
void sms_display_show_external_info(const char* romTitle, bool isGG);
#endif
