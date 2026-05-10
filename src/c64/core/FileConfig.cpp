/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>
 Cardputer adapter changes Copyright (C) 2026 CardputerGBC-Dual contributors

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.
*/
#include "FileConfig.h"

bool FileConfig::loadConfigCalled = false;
RootConfig FileConfig::cfg = {};

void FileConfig::loadConfig(FileDriver &fd, const std::string &filename)
{
  (void)fd;
  (void)filename;
  loadConfigCalled = true;
  cfg = {};
}

std::string FileConfig::getAutostartGame()
{
  return cfg.autostart;
}

std::string FileConfig::getSdlKeyboardLayout()
{
  return cfg.sdlkeyboardlayout;
}

std::vector<JoystickOnlyTextKeycode> FileConfig::getJoystickOnlyKeycodes()
{
  return cfg.joystickOnly.keycodes;
}
