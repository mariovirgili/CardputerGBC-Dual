/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#ifndef C64_DISPLAY_FACTORY_H
#define C64_DISPLAY_FACTORY_H

#include "../Config.h"
#include "DisplayDriver.h"

#if defined(USE_CARDPUTER_DISPLAY)
#include "c64_cardputer_display.h"
#else
#error "no valid C64 display driver defined"
#endif

namespace Display {
DisplayDriver *create() {
#if defined(USE_CARDPUTER_DISPLAY)
  return new C64CardputerDisplay();
#endif
}
} // namespace Display

#endif // C64_DISPLAY_FACTORY_H
