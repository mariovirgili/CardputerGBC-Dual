/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#ifndef C64_CORE_CONFIG_H
#define C64_CORE_CONFIG_H

#include <cstdint>

#define USE_CARDPUTER_DISPLAY
#define USE_CARDPUTER_KEYBOARD
#define USE_CARDPUTER_JOYSTICK
#define USE_NOFS
#define USE_NOSOUND

#define AUDIO_SAMPLE_RATE 44100

struct Config {
  static const uint8_t REFRESHDELAY = 0;
  static constexpr double HEURISTIC_PERFORMANCE_FACTOR = 1.0;

  static const uint16_t LCDWIDTH = 320;
  static const uint16_t LCDHEIGHT = 240;

  static constexpr const char *PATH = "";
  static constexpr const char *CONFIGFILE = ".config.json";
};

#endif // C64_CORE_CONFIG_H
