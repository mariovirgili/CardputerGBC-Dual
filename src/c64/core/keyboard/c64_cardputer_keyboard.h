/*
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#ifndef C64_CARDPUTER_KEYBOARD_H
#define C64_CARDPUTER_KEYBOARD_H

#include "KeyboardDriver.h"
#include <atomic>

class C64CardputerKeyboard : public KeyboardDriver {
public:
  void init() override;
  void scanKeyboard() override;
  uint8_t getKBCodeDC01() override;
  uint8_t getKBCodeDC00() override;
  uint8_t getShiftctrlcode() override;
  uint8_t getKBJoyValue() override;
};

void c64_cardputer_keyboard_set_joystick(uint8_t value);

#endif // C64_CARDPUTER_KEYBOARD_H
