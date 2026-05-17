#include "snes_input.h"

#include <M5Cardputer.h>
#include "compat/arduino_compat.h"
#include "share/input.h"
#include "esp_heap_caps.h"

extern "C" {
    #include "snes9x/snes9x.h"
}

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern bool snes_interlace_lock_parity;
static volatile uint32_t s_lastInputMask = 0;

#ifndef SNES_NO_THREADED_INPUT

static TaskHandle_t s_inputTaskHandle = nullptr;
static volatile uint32_t s_inputMask  = 0;

uint32_t snes_input_compute_mask()
{
    uint32_t mask = 0;

    if (share::shouldPollInput() == false) {
        return s_lastInputMask;
    }

    M5Cardputer.update();
    Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

    // volume / brightness / quit / etc
    share::checkCommonInput(ks);

    // ================== SCREEN MODE ==================
    if (M5Cardputer.Keyboard.isChange() &&
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {
        snes_interlace_lock_parity = !snes_interlace_lock_parity;
        return s_lastInputMask;
    }

    // ================== ZOOM ==================
    // Fn + Right / Left pour éviter de casser les directions SNES
    if (ks.fn &&
        (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1) ||
         M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_2))) {
        snes_zoom_in();
        return s_lastInputMask;
    }

    if (ks.fn &&
        (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1) ||
         M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_2))) {
        snes_zoom_out();
        return s_lastInputMask;
    }

    // ================== I2C PAD ==================
    if (share::hasI2cPad()) {
        int i2cPad = share::pollI2cPad();

        if (i2cPad & share::PAD_LEFT)  mask |= SNES_LEFT_MASK;
        if (i2cPad & share::PAD_RIGHT) mask |= SNES_RIGHT_MASK;
        if (i2cPad & share::PAD_UP)    mask |= SNES_UP_MASK;
        if (i2cPad & share::PAD_DOWN)  mask |= SNES_DOWN_MASK;
        if (i2cPad & share::PAD_A)     mask |= SNES_B_MASK;
    }

    // ================== DIRECTIONS ==================
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_2)) {
        mask |= SNES_LEFT_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_2)) {
        mask |= SNES_RIGHT_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_2)) {
        mask |= SNES_UP_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_2) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_3)) {
        mask |= SNES_DOWN_MASK;
    }

    // ================== BOUTONS SNES ==================
    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_B)) {
        mask |= SNES_B_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('o')) {
        mask |= SNES_Y_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('p')) {
        mask |= SNES_X_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_1)) {
        mask |= SNES_A_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('i')) {
        mask |= SNES_TL_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('j')) {
        mask |= SNES_TR_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_START)) {
        mask |= SNES_START_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_SELECT)) {
        mask |= SNES_SELECT_MASK;
    }

    s_lastInputMask = mask;
    return mask;
}

static void snes_input_task(void *arg)
{
    (void)arg;

    for (;;) {
        s_inputMask = snes_input_compute_mask();
        vTaskDelay(pdMS_TO_TICKS(32));
    }
}

extern "C" void snes_input_start(void)
{
    if (s_inputTaskHandle != nullptr) {
        return;
    }

    xTaskCreatePinnedToCore(
        snes_input_task,
        "snes_input",
        2048,
        nullptr,
        0,
        &s_inputTaskHandle,
        0
    );
}

extern "C" void snes_input_stop(void)
{
    if (s_inputTaskHandle == nullptr) {
        return;
    }

    vTaskDelete(s_inputTaskHandle);
    s_inputTaskHandle = nullptr;
    s_inputMask       = 0;
}

extern "C" uint32_t snes_input_poll(void)
{
    return s_inputMask;
}

#else

void snes_input_start()
{
}

void snes_input_stop()
{
}

uint32_t snes_input_poll()
{
    uint32_t mask = 0;

    if (share::shouldPollInput() == false) {
        return s_lastInputMask;
    }

    M5Cardputer.update();
    Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

    share::checkCommonInput(ks);

    if (M5Cardputer.Keyboard.isChange() &&
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_SCREEN_TOGGLE)) {
        snes_interlace_lock_parity = !snes_interlace_lock_parity;
        return s_lastInputMask;
    }

    if (share::hasI2cPad()) {
        int i2cPad = share::pollI2cPad();

        if (i2cPad & share::PAD_LEFT)  mask |= SNES_LEFT_MASK;
        if (i2cPad & share::PAD_RIGHT) mask |= SNES_RIGHT_MASK;
        if (i2cPad & share::PAD_UP)    mask |= SNES_UP_MASK;
        if (i2cPad & share::PAD_DOWN)  mask |= SNES_DOWN_MASK;
        if (i2cPad & share::PAD_A)     mask |= SNES_B_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_LEFT_2)) {
        mask |= SNES_LEFT_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_RIGHT_2)) {
        mask |= SNES_RIGHT_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_UP_2)) {
        mask |= SNES_UP_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_1) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_2) ||
        M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_DOWN_3)) {
        mask |= SNES_DOWN_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_B)) {
        mask |= SNES_B_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('o')) {
        mask |= SNES_Y_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('p')) {
        mask |= SNES_X_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_A_1)) {
        mask |= SNES_A_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('i')) {
        mask |= SNES_TL_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('j')) {
        mask |= SNES_TR_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_START)) {
        mask |= SNES_START_MASK;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(CARDPUTER_BTN_SELECT)) {
        mask |= SNES_SELECT_MASK;
    }

    s_lastInputMask = mask;
    return mask;
}

#endif