/*
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#ifndef C64_CARDPUTER_JOYSTICK_H
#define C64_CARDPUTER_JOYSTICK_H

#include "JoystickDriver.h"

class C64CardputerJoystick : public JoystickDriver {
public:
  void init() override {}
  uint8_t getValue() override;
  bool getFire2() override;
  bool getJoyOnlyModeButton() override;
};

void c64_cardputer_joystick_set_state(uint8_t value, bool fire2);

#endif // C64_CARDPUTER_JOYSTICK_H
