/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#ifndef C64_KEYBOARD_FACTORY_H
#define C64_KEYBOARD_FACTORY_H

#include "../Config.h"
#include "KeyboardDriver.h"

#if defined(USE_CARDPUTER_KEYBOARD)
#include "c64_cardputer_keyboard.h"
#else
#error "no valid C64 keyboard driver defined"
#endif

namespace Keyboard {
KeyboardDriver *create() {
#if defined(USE_CARDPUTER_KEYBOARD)
  return new C64CardputerKeyboard();
#endif
}
} // namespace Keyboard

#endif // C64_KEYBOARD_FACTORY_H
