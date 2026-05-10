/*
 Cardputer adapter code Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#include "c64_cardputer_keyboard.h"

#include <M5Cardputer.h>
#include <atomic>
#include <cctype>

#include "C64Keycodes.h"

namespace {
std::atomic<uint8_t> s_dc00{0xff};
std::atomic<uint8_t> s_dc01{0xff};
std::atomic<uint8_t> s_shiftCtrl{0};
std::atomic<uint8_t> s_joy{0xff};

bool key_pressed(char c)
{
  return M5Cardputer.Keyboard.isKeyPressed(c);
}

CodeTripleS map_cardputer_key()
{
  if (key_pressed('\n') || key_pressed('\r')) return C64_KEYCODE_RETURN;
  if (key_pressed('\b')) return C64_KEYCODE_DEL;
  if (key_pressed(' ')) return C64_KEYCODE_SPACE;
  if (key_pressed(',')) return C64_KEYCODE_COMMA;
  if (key_pressed('.')) return C64_KEYCODE_PERIOD;
  if (key_pressed('/')) return C64_KEYCODE_SLASH;
  if (key_pressed(';')) return C64_KEYCODE_SEMICOLON;
  if (key_pressed(':')) return C64_KEYCODE_COLON;
  if (key_pressed('-')) return C64_KEYCODE_MINUS;
  if (key_pressed('+')) return C64_KEYCODE_PLUS;
  if (key_pressed('=')) return C64_KEYCODE_EQUALS;
  if (key_pressed('@')) return C64_KEYCODE_AT;
  if (key_pressed('*')) return C64_KEYCODE_ASTERISK;

  if (key_pressed('1')) return C64_KEYCODE_1;
  if (key_pressed('2')) return C64_KEYCODE_2;
  if (key_pressed('3')) return C64_KEYCODE_3;
  if (key_pressed('4')) return C64_KEYCODE_4;
  if (key_pressed('5')) return C64_KEYCODE_5;
  if (key_pressed('6')) return C64_KEYCODE_6;
  if (key_pressed('7')) return C64_KEYCODE_7;
  if (key_pressed('8')) return C64_KEYCODE_8;
  if (key_pressed('9')) return C64_KEYCODE_9;
  if (key_pressed('0')) return C64_KEYCODE_0;

  if (key_pressed('a') || key_pressed('A')) return C64_KEYCODE_A;
  if (key_pressed('b') || key_pressed('B')) return C64_KEYCODE_B;
  if (key_pressed('c') || key_pressed('C')) return C64_KEYCODE_C;
  if (key_pressed('d') || key_pressed('D')) return C64_KEYCODE_D;
  if (key_pressed('e') || key_pressed('E')) return C64_KEYCODE_E;
  if (key_pressed('f') || key_pressed('F')) return C64_KEYCODE_F;
  if (key_pressed('g') || key_pressed('G')) return C64_KEYCODE_G;
  if (key_pressed('h') || key_pressed('H')) return C64_KEYCODE_H;
  if (key_pressed('i') || key_pressed('I')) return C64_KEYCODE_I;
  if (key_pressed('j') || key_pressed('J')) return C64_KEYCODE_J;
  if (key_pressed('k') || key_pressed('K')) return C64_KEYCODE_K;
  if (key_pressed('l') || key_pressed('L')) return C64_KEYCODE_L;
  if (key_pressed('m') || key_pressed('M')) return C64_KEYCODE_M;
  if (key_pressed('n') || key_pressed('N')) return C64_KEYCODE_N;
  if (key_pressed('o') || key_pressed('O')) return C64_KEYCODE_O;
  if (key_pressed('p') || key_pressed('P')) return C64_KEYCODE_P;
  if (key_pressed('q') || key_pressed('Q')) return C64_KEYCODE_Q;
  if (key_pressed('r') || key_pressed('R')) return C64_KEYCODE_R;
  if (key_pressed('s') || key_pressed('S')) return C64_KEYCODE_S;
  if (key_pressed('t') || key_pressed('T')) return C64_KEYCODE_T;
  if (key_pressed('u') || key_pressed('U')) return C64_KEYCODE_U;
  if (key_pressed('v') || key_pressed('V')) return C64_KEYCODE_V;
  if (key_pressed('w') || key_pressed('W')) return C64_KEYCODE_W;
  if (key_pressed('x') || key_pressed('X')) return C64_KEYCODE_X;
  if (key_pressed('y') || key_pressed('Y')) return C64_KEYCODE_Y;
  if (key_pressed('z') || key_pressed('Z')) return C64_KEYCODE_Z;

  return {0xff, 0xff, 0x00};
}
} // namespace

void c64_cardputer_keyboard_set_joystick(uint8_t value)
{
  s_joy.store(value, std::memory_order_release);
}

void C64CardputerKeyboard::init() {}

void C64CardputerKeyboard::scanKeyboard()
{
  const CodeTripleS key = map_cardputer_key();
  s_dc00.store(key.dc00, std::memory_order_release);
  s_dc01.store(key.dc01, std::memory_order_release);
  s_shiftCtrl.store(key.modifier, std::memory_order_release);
}

uint8_t C64CardputerKeyboard::getKBCodeDC01()
{
  return s_dc01.load(std::memory_order_acquire);
}

uint8_t C64CardputerKeyboard::getKBCodeDC00()
{
  return s_dc00.load(std::memory_order_acquire);
}

uint8_t C64CardputerKeyboard::getShiftctrlcode()
{
  return s_shiftCtrl.load(std::memory_order_acquire);
}

uint8_t C64CardputerKeyboard::getKBJoyValue()
{
  return s_joy.load(std::memory_order_acquire);
}
