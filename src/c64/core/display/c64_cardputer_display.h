/*
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#ifndef C64_CARDPUTER_DISPLAY_H
#define C64_CARDPUTER_DISPLAY_H

#include "DisplayDriver.h"

class C64CardputerDisplay : public DisplayDriver {
public:
  void init() override;
  void drawFrame(uint8_t frameColor) override;
  void drawBitmap(const uint8_t *bitmap, const uint8_t *vicreg) override;
};

void c64_cardputer_display_set_fps(uint16_t fps10);
void c64_cardputer_display_set_menu(bool visible);

#endif // C64_CARDPUTER_DISPLAY_H
