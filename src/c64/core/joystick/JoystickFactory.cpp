/*
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#include "JoystickFactory.h"

#include "../Config.h"
#include "c64_cardputer_joystick.h"

namespace Joystick {
JoystickDriver *create()
{
  return new C64CardputerJoystick();
}
} // namespace Joystick
