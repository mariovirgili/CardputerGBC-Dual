/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#ifndef C64_SOUND_FACTORY_H
#define C64_SOUND_FACTORY_H

#include "../Config.h"
#include "SoundDriver.h"

#if defined(USE_NOSOUND)
#include "NoSound.h"
#else
#error "no valid C64 sound driver defined"
#endif

namespace Sound {
SoundDriver *create() {
#if defined(USE_NOSOUND)
  return new NoSound();
#endif
}
} // namespace Sound

#endif // C64_SOUND_FACTORY_H
