/*
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#include "c64_cardputer_joystick.h"

#include <atomic>

namespace {
std::atomic<uint8_t> s_state{0xff};
std::atomic<bool> s_fire2{false};
}

void c64_cardputer_joystick_set_state(uint8_t value, bool fire2)
{
  s_state.store(value, std::memory_order_release);
  s_fire2.store(fire2, std::memory_order_release);
}

uint8_t C64CardputerJoystick::getValue()
{
  return s_state.load(std::memory_order_acquire);
}

bool C64CardputerJoystick::getFire2()
{
  return s_fire2.load(std::memory_order_acquire);
}

bool C64CardputerJoystick::getJoyOnlyModeButton()
{
  return getFire2();
}
