/*
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#include "c64_cardputer_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <TFT_eSPI.h>

#include "../../../share/display_target.h"
#include "../../../tft_setup.h"

namespace {
constexpr int kC64Width = 320;
constexpr int kC64Height = 200;
constexpr int kExternalWidth = 320;
constexpr int kExternalHeight = 240;

TFT_eSPI s_externalTft;
bool s_externalReady = false;
uint16_t s_fps10 = 0;
bool s_menuVisible = false;
uint8_t s_lastFrameColor = 0;

TFT_eSPI& c64_external_tft()
{
  if (!s_externalReady) {
    emu_set_aux_screen_locked(false);
    s_externalTft.begin();
    s_externalTft.setRotation(3);
    s_externalReady = true;
  }
  return s_externalTft;
}

uint16_t c64_color(uint8_t color)
{
  static constexpr uint16_t kColors[16] = {
      0x0000, 0xffff, 0x8000, 0xa7fc,
      0xc218, 0x064a, 0x0014, 0xe74e,
      0xd42a, 0x6200, 0xfbae, 0x3186,
      0x73ae, 0xa7ec, 0x043f, 0xb5d6};
  return kColors[color & 0x0f];
}

void draw_fps_external(TFT_eSPI& tft)
{
  if (s_fps10 == 0 || s_menuVisible) {
    return;
  }

  char text[16];
  snprintf(text, sizeof(text), "%u.%u", s_fps10 / 10, s_fps10 % 10);
  tft.fillRect(273, 2, 45, 13, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(text, 276, 4, 1);
}

void draw_fps_internal()
{
  if (s_fps10 == 0 || s_menuVisible) {
    return;
  }

  char text[16];
  snprintf(text, sizeof(text), "%u.%u", s_fps10 / 10, s_fps10 % 10);
  auto& display = M5Cardputer.Display;
  display.fillRect(display.width() - 35, 1, 34, 10, TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString(text, display.width() - 33, 2, &fonts::Font0);
}
} // namespace

void c64_cardputer_display_set_fps(uint16_t fps10)
{
  s_fps10 = fps10;
}

void c64_cardputer_display_set_menu(bool visible)
{
  s_menuVisible = visible;
}

void C64CardputerDisplay::init()
{
  s_lastFrameColor = 0;
  if (g_emu_display_target == EMU_DISPLAY_EXTERNAL) {
    auto& tft = c64_external_tft();
    tft.fillScreen(TFT_BLACK);
    return;
  }
  M5Cardputer.Display.fillScreen(TFT_BLACK);
}

void C64CardputerDisplay::drawFrame(uint8_t frameColor)
{
  s_lastFrameColor = frameColor & 0x0f;
  const uint16_t color = c64_color(s_lastFrameColor);

  if (g_emu_display_target == EMU_DISPLAY_EXTERNAL) {
    auto& tft = c64_external_tft();
    tft.fillRect(0, 0, kExternalWidth, 20, color);
    tft.fillRect(0, 220, kExternalWidth, 20, color);
    draw_fps_external(tft);
    return;
  }

  draw_fps_internal();
}

void C64CardputerDisplay::drawBitmap(const uint8_t *bitmap, const uint8_t *vicreg)
{
  (void)vicreg;
  if (!bitmap) {
    return;
  }

  if (g_emu_display_target == EMU_DISPLAY_EXTERNAL) {
    auto& tft = c64_external_tft();
    uint16_t line[kC64Width];
    for (int y = 0; y < kC64Height; ++y) {
      const uint8_t* src = bitmap + y * kC64Width;
      for (int x = 0; x < kC64Width; ++x) {
        line[x] = c64_color(src[x]);
      }
      tft.pushImage(0, y + 20, kC64Width, 1, line);
    }
    return;
  }

  auto& display = M5Cardputer.Display;
  constexpr int outW = 240;
  constexpr int outH = 135;
  uint16_t line[outW];
  const int x0 = (display.width() - outW) / 2;
  const int y0 = (display.height() - outH) / 2;
  for (int y = 0; y < outH; ++y) {
    const int srcY = (y * kC64Height) / outH;
    const uint8_t* src = bitmap + srcY * kC64Width;
    for (int x = 0; x < outW; ++x) {
      const int srcX = (x * kC64Width) / outW;
      line[x] = c64_color(src[srcX]);
    }
    display.pushImage(x0, y0 + y, outW, 1, line);
  }
}
