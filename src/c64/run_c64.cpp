#include "run_c64.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cardputer/CardputerInput.h"
#include "cardputer/CardputerView.h"
#include "cardputer/SdService.h"
#include "cardputer/VerticalSelector.h"
#include "c64_config.h"
#include "core/C64Sys.h"
#include "core/ExternalCmds.h"
#include "core/platform/PlatformFactory.h"
#include "core/platform/PlatformManager.h"
#include "core/roms/charset.h"
#include "core/display/c64_cardputer_display.h"
#include "core/joystick/c64_cardputer_joystick.h"
#include "core/keyboard/c64_cardputer_keyboard.h"
#include "core/keyboard/C64Keycodes.h"
#include "last_game.h"
#include "share/emu_controls.h"
#include "share/input.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
struct C64Runtime {
  C64Sys* c64;
  volatile bool stop;
};

C64Runtime s_runtime = {};
TaskHandle_t s_c64CpuTask = nullptr;
TaskHandle_t s_c64DisplayTask = nullptr;

void c64_mark_quit_game()
{
  Preferences prefs;
  prefs.begin("cardputer_emu", false);
  prefs.putBool("quit_game", true);
  prefs.end();
}

void c64_cpu_task(void* arg)
{
  auto* runtime = static_cast<C64Runtime*>(arg);
  runtime->c64->run();
  vTaskDelete(nullptr);
}

void c64_display_task(void* arg)
{
  auto* runtime = static_cast<C64Runtime*>(arg);
  while (!runtime->stop) {
    runtime->c64->vic.refresh();
    taskYIELD();
  }
  vTaskDelete(nullptr);
}

uint16_t c64_load_prg_to_ram(uint8_t* ram, const uint8_t* data, size_t len)
{
  if (!ram || !data || len <= 2) {
    return 0;
  }

  const uint16_t loadAddr = static_cast<uint16_t>(data[0] | (data[1] << 8));
  const size_t payloadLen = std::min<size_t>(len - 2, 0x10000u - loadAddr);
  std::memcpy(ram + loadAddr, data + 2, payloadLen);
  return static_cast<uint16_t>(loadAddr + payloadLen);
}

void c64_queue_run_command(C64Sys& c64, uint8_t* ram, uint16_t endAddr)
{
  if (endAddr == 0) {
    return;
  }

  c64.externalCmds->setVarTab(endAddr);
  c64.joystickOnlyModeState = JoystickOnlyModeState::RUN;
  ram[0xd3] = 0;
  ram[0x0277] = 'R';
  ram[0x0278] = 'U';
  ram[0x0279] = 'N';
  ram[0x027A] = ':';
  ram[0x027B] = 0x0d;
  ram[0x00C6] = 5;
}

uint8_t c64_current_joystick_value()
{
  uint8_t value = 0xff;
  if (share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::Up)) {
    value &= static_cast<uint8_t>(~(1u << 0));
  }
  if (share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::Down)) {
    value &= static_cast<uint8_t>(~(1u << 1));
  }
  if (share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::Left)) {
    value &= static_cast<uint8_t>(~(1u << 2));
  }
  if (share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::Right)) {
    value &= static_cast<uint8_t>(~(1u << 3));
  }
  if (share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::A)) {
    value &= static_cast<uint8_t>(~(1u << 4));
  }

  const uint32_t pad = share::pollI2cPad();
  if (pad & share::PAD_UP) value &= static_cast<uint8_t>(~(1u << 0));
  if (pad & share::PAD_DOWN) value &= static_cast<uint8_t>(~(1u << 1));
  if (pad & share::PAD_LEFT) value &= static_cast<uint8_t>(~(1u << 2));
  if (pad & share::PAD_RIGHT) value &= static_cast<uint8_t>(~(1u << 3));
  if (pad & share::PAD_A) value &= static_cast<uint8_t>(~(1u << 4));
  return value;
}

bool c64_fire2_pressed()
{
  return share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::B);
}

bool c64_button_pressed_for_ms(uint32_t heldMs)
{
  const uint32_t start = millis();
  while (M5Cardputer.BtnA.isPressed()) {
    M5Cardputer.update();
    if (millis() - start >= heldMs) {
      return true;
    }
    delay(5);
  }
  return false;
}

void c64_show_runtime_menu(C64Sys& c64, SdService& sd, CardputerView& display, CardputerInput& input)
{
  c64_cardputer_display_set_menu(true);
  VerticalSelector selector(display, input);
  int selectedIndex = 0;
  for (;;) {
    const bool fpsHud = c64_config_get_fps_hud_enabled();
    std::vector<std::string> labels = {
      "Resume",
      "Joystick port",
      "FPS HUD",
      "Config Keys",
      "Reset C64",
      "Quit",
    };
    std::vector<std::string> values = {
      "",
      c64.joystickmode == 1 ? "PORT 1" : "PORT 2",
      fpsHud ? "ON" : "OFF",
      "",
      "",
      "",
    };

    const int selection = selector.select("C64 CONFIG MENU",
                                          labels,
                                          false,
                                          false,
                                          values,
                                          {},
                                          false,
                                          true,
                                          true,
                                          selectedIndex);
    selectedIndex = selection >= 0 ? selection : 0;
    if (selection <= 0) {
      c64_cardputer_display_set_menu(false);
      input.flushInput(150);
      return;
    }

    switch (selection) {
      case 1:
        c64.joystickmode = c64.joystickmode == 1 ? 2 : 1;
        break;
      case 2:
        c64_config_set_fps_hud_enabled(!fpsHud, true);
        c64_cardputer_display_set_fps(c64_config_get_fps_hud_enabled() ? 1 : 0);
        break;
      case 3:
        share::emuControlsEdit(sd, share::EmuProfile::C64, display, input);
        break;
      case 4:
        c64.cpuhalted = true;
        c64.initMemAndRegs();
        c64.vic.initVarsAndRegs();
        c64.cia1.init(true);
        c64.cia2.init(false);
        c64.sid.init();
        c64.floppy.init(8);
        c64.cpuhalted = false;
        break;
      case 5:
        c64_cardputer_display_set_menu(false);
        c64_mark_quit_game();
        delay(100);
        esp_restart();
        break;
      default:
        break;
    }
  }
}
} // namespace

void run_c64_prg(const uint8_t* prgData, size_t prgLen, const char* prgName, SdService& sd)
{
  printf("[C64] launch PRG: name=%s size=%u\n",
         prgName && prgName[0] ? prgName : "(unnamed)",
         static_cast<unsigned>(prgLen));

  CardputerView display;
  CardputerInput input;
  display.initialize();
  display.topBar("STARTING C64", false, false);
  display.subMessage(prgName && prgName[0] ? prgName : "C64 PRG", "Preparing core", 0);

  share::emuControlsLoad(sd, share::EmuProfile::C64);
  c64_config_load_fps_hud_enabled();

  PlatformManager::initialize(PlatformNS::create());

  uint8_t* ram = static_cast<uint8_t*>(heap_caps_malloc(1 << 16, MALLOC_CAP_8BIT));
  if (!ram) {
    display.topBar("C64 RAM ERROR", false, false);
    display.subMessage("Could not allocate 64KB", 0);
    while (true) delay(1000);
  }
  std::memset(ram, 0, 1 << 16);

  static C64Sys c64;
  c64.init(ram, charset_rom);
  c64.joystickmode = 2;

  const uint16_t endAddr = c64_load_prg_to_ram(ram, prgData, prgLen);
  c64_queue_run_command(c64, ram, endAddr);

  s_runtime.c64 = &c64;
  s_runtime.stop = false;

  xTaskCreatePinnedToCore(c64_cpu_task, "c64_cpu", 10000, &s_runtime, 19, &s_c64CpuTask, 1);
  xTaskCreatePinnedToCore(c64_display_task, "c64_video", 6144, &s_runtime, 18, &s_c64DisplayTask, 0);

  uint32_t lastFpsMs = millis();
  c64.vic.cntRefreshs.store(0, std::memory_order_release);
  c64_cardputer_display_set_fps(0);

  bool goWasPressed = false;
  uint32_t goPressedMs = 0;
  bool menuOpened = false;

  for (;;) {
    M5Cardputer.update();
    const auto status = M5Cardputer.Keyboard.keysState();

    const uint8_t joy = c64_current_joystick_value();
    const bool fire2 = c64_fire2_pressed();
    c64_cardputer_joystick_set_state(joy, fire2);
    c64_cardputer_keyboard_set_joystick(joy);

    if (M5Cardputer.BtnA.isPressed()) {
      if (!goWasPressed) {
        goPressedMs = millis();
        goWasPressed = true;
        menuOpened = false;
      } else if (!menuOpened && millis() - goPressedMs >= 700) {
        menuOpened = true;
        c64_show_runtime_menu(c64, sd, display, input);
      }
    } else if (goWasPressed) {
      if (!menuOpened && millis() - goPressedMs < 700) {
        c64_mark_quit_game();
        delay(100);
        esp_restart();
      }
      goWasPressed = false;
    }

    if (share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::Start)) {
      c64.actInGameKeycode = C64_KEYCODE_BREAK;
      c64.actInGameKeycodeChosen.store(true, std::memory_order_release);
      c64.actInGameKeycodeCnt.store(3, std::memory_order_release);
    }
    if (share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::Select)) {
      c64.restorenmi = true;
    }
    if (share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::Option)) {
      c64.actInGameKeycode = C64_KEYCODE_F1;
      c64.actInGameKeycodeChosen.store(true, std::memory_order_release);
      c64.actInGameKeycodeCnt.store(3, std::memory_order_release);
    }
    if (share::emuControlPressed(share::EmuProfile::C64, share::EmuAction::Option2)) {
      c64.actInGameKeycode = C64_KEYCODE_F3;
      c64.actInGameKeycodeChosen.store(true, std::memory_order_release);
      c64.actInGameKeycodeCnt.store(3, std::memory_order_release);
    }

    share::checkCommonInput(status);

    const uint32_t now = millis();
    if (now - lastFpsMs >= 1000) {
      const uint8_t frames = c64.vic.cntRefreshs.exchange(0, std::memory_order_acq_rel);
      c64_cardputer_display_set_fps(c64_config_get_fps_hud_enabled()
                                        ? static_cast<uint16_t>(frames * 10)
                                        : 0);
      lastFpsMs = now;
    }

    delay(2);
  }
}
