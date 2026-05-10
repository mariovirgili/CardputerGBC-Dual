/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#ifndef C64_JOYSTICK_FACTORY_H
#define C64_JOYSTICK_FACTORY_H

#include "JoystickDriver.h"

namespace Joystick {
JoystickDriver *create();
} // namespace Joystick

#endif // C64_JOYSTICK_FACTORY_H
