#ifndef VIDEOPAC_DISPLAY_H
#define VIDEOPAC_DISPLAY_H

#include <cstdint>
#include <cstddef>

void videopac_display_init(bool useExternal);
void videopac_display_render(const uint16_t* buffer, int width, int height, int pitchPixels, bool useExternal);
void videopac_display_render_indexed(const uint8_t* buffer,
                                     int width,
                                     int height,
                                     int pitchPixels,
                                     const uint16_t palette[256],
                                     bool useExternal);
void videopac_display_show_external_info(const char* romTitle, const char* biosName);
void videopac_display_show_runtime_menu(bool useExternal);
void videopac_display_show_input_mode_overlay(bool keyboardOnlyMode, bool useExternal);
void videopac_display_shutdown(void);

#endif // VIDEOPAC_DISPLAY_H
