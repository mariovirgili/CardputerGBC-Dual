/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 for more details.

 For the complete text of the GNU General Public License see
 http://www.gnu.org/licenses/.
*/
#ifndef FILECONFIG_H
#define FILECONFIG_H

#include "JoystickOnlyTextKeycode.h"
#include "fs/FileDriver.h"
#include "keyboard/C64Keycodes.h"
#include "keyboard/CodeTripleDef.h"
#include <string>
#include <vector>

struct JoystickOnlyConfig {
  std::vector<JoystickOnlyTextKeycode> keycodes;
};
struct RootConfig {
  int version = 1;
  std::string autostart;
  std::string sdlkeyboardlayout;
  JoystickOnlyConfig joystickOnly;
};

class FileConfig {
private:
  static bool loadConfigCalled;
  static RootConfig cfg;

public:
  static void loadConfig(FileDriver &fd, const std::string &filename);
  static std::string getAutostartGame();
  static std::string getSdlKeyboardLayout();
  static std::vector<JoystickOnlyTextKeycode> getJoystickOnlyKeycodes();
};

#endif // FILECONFIG_H
