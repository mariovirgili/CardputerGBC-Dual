#include "ws_input.h"
#include <M5Cardputer.h>
#include <M5GFX.h>
#include <algorithm>
#include <cstring>
#include <stdint.h>
#include "esp_system.h"
#include "compat/preferences_compat.h"
#include "game_save.h"
#include "share/input.h"
#include "share/boot_log.h"
#include "utility/Adafruit_TCA8418/Adafruit_TCA8418_registers.h"
#include "ws_save.h"
#include "ws_state.h"

extern bool ws_fullscreen;
extern int  ws_zoomPercent;
extern uint32_t lastPadState;

static volatile uint16_t s_cachedState[2] = {0, 0};
static bool s_keyDown[4][14] = {};

static constexpr int WS_KBD_SDA = 8;
static constexpr int WS_KBD_SCL = 9;
static constexpr uint8_t WS_KBD_ADDR = 0x34;
static constexpr uint32_t WS_KBD_I2C_DELAY_US = 2;

static inline void ws_kbd_delay()
{
  m5gfx::delayMicroseconds(WS_KBD_I2C_DELAY_US);
}

static inline void ws_kbd_sda(bool high)
{
  high ? m5gfx::gpio_hi(WS_KBD_SDA) : m5gfx::gpio_lo(WS_KBD_SDA);
}

static inline void ws_kbd_scl(bool high)
{
  high ? m5gfx::gpio_hi(WS_KBD_SCL) : m5gfx::gpio_lo(WS_KBD_SCL);
}

static void ws_kbd_i2c_start()
{
  ws_kbd_sda(true);
  ws_kbd_scl(true);
  ws_kbd_delay();
  ws_kbd_sda(false);
  ws_kbd_delay();
  ws_kbd_scl(false);
}

static void ws_kbd_i2c_stop()
{
  ws_kbd_sda(false);
  ws_kbd_delay();
  ws_kbd_scl(true);
  ws_kbd_delay();
  ws_kbd_sda(true);
  ws_kbd_delay();
}

static bool ws_kbd_i2c_write(uint8_t value)
{
  for (int bit = 7; bit >= 0; --bit) {
    ws_kbd_sda((value >> bit) & 1);
    ws_kbd_delay();
    ws_kbd_scl(true);
    ws_kbd_delay();
    ws_kbd_scl(false);
  }

  ws_kbd_sda(true);
  ws_kbd_delay();
  ws_kbd_scl(true);
  ws_kbd_delay();
  const bool ack = !m5gfx::gpio_in(WS_KBD_SDA);
  ws_kbd_scl(false);
  return ack;
}

static uint8_t ws_kbd_i2c_read(bool ack)
{
  uint8_t value = 0;
  ws_kbd_sda(true);

  for (int bit = 7; bit >= 0; --bit) {
    ws_kbd_delay();
    ws_kbd_scl(true);
    ws_kbd_delay();
    if (m5gfx::gpio_in(WS_KBD_SDA)) {
      value |= (uint8_t)(1u << bit);
    }
    ws_kbd_scl(false);
  }

  ws_kbd_sda(!ack);
  ws_kbd_delay();
  ws_kbd_scl(true);
  ws_kbd_delay();
  ws_kbd_scl(false);
  ws_kbd_sda(true);
  return value;
}

static bool ws_kbd_read_register(uint8_t reg, uint8_t *data, size_t len)
{
  if (!data || len == 0) {
    return false;
  }

  ws_kbd_i2c_start();
  if (!ws_kbd_i2c_write((uint8_t)(WS_KBD_ADDR << 1)) || !ws_kbd_i2c_write(reg)) {
    ws_kbd_i2c_stop();
    return false;
  }

  ws_kbd_i2c_start();
  if (!ws_kbd_i2c_write((uint8_t)((WS_KBD_ADDR << 1) | 1))) {
    ws_kbd_i2c_stop();
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    data[i] = ws_kbd_i2c_read(i + 1 < len);
  }

  ws_kbd_i2c_stop();
  return true;
}

static bool ws_kbd_read8(uint8_t reg, uint8_t &value)
{
  value = 0;
  return ws_kbd_read_register(reg, &value, 1);
}

static void ws_kbd_apply_event(uint8_t eventRaw)
{
  if ((eventRaw & 0x7F) == 0) {
    return;
  }

  uint8_t raw = (eventRaw & 0x7F) - 1;
  uint8_t tcaRow = raw / 10;
  uint8_t tcaCol = raw % 10;
  uint8_t col = tcaRow * 2;
  if (tcaCol > 3) {
    col++;
  }
  uint8_t row = (tcaCol + 4) % 4;

  if (row < 4 && col < 14) {
    s_keyDown[row][col] = (eventRaw & 0x80) != 0;
  }
}

static void ws_kbd_update()
{
  uint8_t ec = 0;
  if (!ws_kbd_read8(TCA8418_REG_KEY_LCK_EC, ec)) {
    return;
  }

  uint8_t eventCount = ec & 0x0F;
  if (eventCount == 0) {
    return;
  }

  if (eventCount > 8) {
    eventCount = 8;
  }

  while (eventCount--) {
    uint8_t eventRaw = 0;
    if (ws_kbd_read8(TCA8418_REG_KEY_EVENT_A, eventRaw)) {
      ws_kbd_apply_event(eventRaw);
    }
  }
}

static bool ws_key_pressed(char c)
{
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 14; ++col) {
      if (!s_keyDown[row][col]) {
        continue;
      }

      const KeyValue_t kv = _key_value_map[row][col];
      if (kv.value_first == c || kv.value_second == c) {
        return true;
      }
    }
  }
  return false;
}

static Keyboard_Class::KeysState ws_build_key_state()
{
  Keyboard_Class::KeysState status;
  status.fn = ws_key_pressed(KEY_FN);
  status.shift = ws_key_pressed(KEY_LEFT_SHIFT);
  status.ctrl = ws_key_pressed(KEY_LEFT_CTRL);
  status.opt = ws_key_pressed(KEY_OPT);
  status.alt = ws_key_pressed(KEY_LEFT_ALT);
  status.del = ws_key_pressed(KEY_BACKSPACE);
  status.enter = ws_key_pressed(KEY_ENTER);
  status.space = ws_key_pressed(' ');
  return status;
}

static void ws_update_btn_a()
{
  const bool pressed = !m5gfx::gpio_in(GPIO_NUM_0);
  M5Cardputer.BtnA.setRawState(millis(), pressed);
}

static void ws_check_quit_button()
{
  if (!M5Cardputer.BtnA.pressedFor(1000)) {
    return;
  }

  BOOT_LOG("INPUT", "BtnA pressedFor(1000) -> quit/restart");
  Preferences prefs;
  prefs.begin("cardputer_emu", false);
  prefs.putBool("quit_game", true);
  prefs.end();

  ws_save_force_flush();
  while (share::gameIsSaving()) {
    delay(1);
  }

  BOOT_LOG("INPUT", "esp_restart from WS input");
  esp_restart();
}

static uint16_t ws_input_compute_state(int mode, uint32_t i2cPad)
{
  uint16_t state = 0;

  // I2C PAD (M5Stack JoyV2)
  if (i2cPad) {
      if (i2cPad & share::PAD_LEFT)  state |= WS_X4; 
      if (i2cPad & share::PAD_RIGHT) state |= WS_X2; 
      if (i2cPad & share::PAD_UP)    state |= WS_X1; 
      if (i2cPad & share::PAD_DOWN)  state |= WS_X3;
      if (i2cPad & share::PAD_A)     state |= WS_A;
  }

  // Directional pad
  const bool left  = ws_key_pressed('e');
  const bool right = ws_key_pressed('z') || ws_key_pressed('s');
  const bool up    = ws_key_pressed('d');
  const bool down  = ws_key_pressed('a');

  // mode == 0 : horizontal
  if (mode == 0) {
    if (left)  state |= WS_X1;
    if (up)    state |= WS_X2;
    if (right) state |= WS_X3;
    if (down)  state |= WS_X4;
  // mode == 1 : vertical 
  } else {
    if (left)  state |= WS_Y1;
    if (up)    state |= WS_Y2;
    if (right) state |= WS_Y3;
    if (down)  state |= WS_Y4;
  }

  // Secondary directional pad 
  const bool a = ws_key_pressed(CARDPUTER_UP_2);
  const bool w = ws_key_pressed(CARDPUTER_DOWN_2);
  const bool d = ws_key_pressed(CARDPUTER_RIGHT_2);
  const bool s = ws_key_pressed(CARDPUTER_LEFT_2);

  if (mode == 0) {
    // In horizontal mode, WASD -> Y cross
    if (a) state |= WS_Y1;
    if (w) state |= WS_Y2;
    if (d) state |= WS_Y3;
    if (s) state |= WS_Y4;
  } else {
    // In vertical mode, WASD -> X cross
    if (a) state |= WS_X1;
    if (w || ws_key_pressed(CARDPUTER_BTN_B)) state |= WS_X2;
    if (d || ws_key_pressed(CARDPUTER_BTN_A_1)) state |= WS_X3;
    if (s) state |= WS_X4;
  }

  //  Boutons
  if (ws_key_pressed(CARDPUTER_BTN_A_1) && mode == 0) state |= WS_A;       // A
  if (ws_key_pressed(CARDPUTER_BTN_A_2) && mode == 0) state |= WS_A;       // A
  if (ws_key_pressed(CARDPUTER_BTN_B) && mode == 0) state |= WS_B;       // B

  if (ws_key_pressed(CARDPUTER_BTN_START)) state |= WS_START;  // START 
  if (ws_key_pressed(CARDPUTER_BTN_SELECT))  state |= WS_OPTION; // OPTION 
  
  return state;
}

extern "C" void ws_input_start(void)
{
  ws_input_tick();
}

extern "C" void ws_input_tick(void)
{
  static bool quitFlushDone = false;
  static bool stateSaveLatch = false;
  static bool stateLoadLatch = false;

  if (!share::shouldPollInput()) {
    return;
  }

  ws_update_btn_a();
  ws_kbd_update();
  Keyboard_Class::KeysState status = ws_build_key_state();

  if (M5Cardputer.BtnA.pressedFor(1000) && !quitFlushDone) {
    ws_save_force_flush();
    quitFlushDone = true;
  }

  (void)status;
  ws_check_quit_button();

  const bool saveStateCombo = status.fn && ws_key_pressed('s');
  const bool loadStateCombo = status.fn && ws_key_pressed('l');
  if (saveStateCombo && !stateSaveLatch) {
    ws_state_request_save();
    stateSaveLatch = true;
  } else if (!saveStateCombo) {
    stateSaveLatch = false;
  }
  if (loadStateCombo && !stateLoadLatch) {
    ws_state_request_load();
    stateLoadLatch = true;
  } else if (!loadStateCombo) {
    stateLoadLatch = false;
  }
  if (saveStateCombo || loadStateCombo) {
    s_cachedState[0] = 0;
    s_cachedState[1] = 0;
    lastPadState = 0;
    return;
  }

  // Zoom / fullscreen toggle
  static bool screenToggleLatch = false;
  const bool screenToggle = ws_key_pressed(CARDPUTER_SCREEN_TOGGLE);
  if (screenToggle && !screenToggleLatch) {
    if (!ws_fullscreen) {
      ws_fullscreen  = true;
      ws_zoomPercent = 100;
    } else {
      ws_zoomPercent += 10;
      if (ws_zoomPercent > 150) {
        ws_zoomPercent = 100;
        ws_fullscreen  = false;
      }
    }
  }
  screenToggleLatch = screenToggle;

  if (status.fn && ws_key_pressed(CARDPUTER_ZOOM_PLUS)) {
    ws_zoomPercent = std::min(150, ws_zoomPercent + 1);
  }

  if (status.fn && ws_key_pressed(CARDPUTER_ZOOM_MINUS)) {
    ws_zoomPercent = std::max(100, ws_zoomPercent - 1);
  }

  const uint32_t i2cPad = share::hasI2cPad() ? share::pollI2cPad() : 0;
  const uint16_t horizontal = ws_input_compute_state(0, i2cPad);
  const uint16_t vertical = ws_input_compute_state(1, i2cPad);
  s_cachedState[0] = horizontal;
  s_cachedState[1] = vertical;
  lastPadState = horizontal;
}

extern "C" void ws_input_stop(void)
{
  s_cachedState[0] = 0;
  s_cachedState[1] = 0;
  memset(s_keyDown, 0, sizeof(s_keyDown));
}

extern "C" int ws_input_poll(int mode)
{
  return (int)s_cachedState[mode ? 1 : 0];
}
