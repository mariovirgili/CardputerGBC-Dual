/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "M5Cardputer.h"
#include "cardputer/CardputerAudio.h"
#include "share/boot_log.h"

using namespace m5;

M5_CARDPUTER M5Cardputer;

void M5_CARDPUTER::begin(bool enableKeyboard)
{
    BOOT_LOG("HW", "M5_CARDPUTER::begin bool keyboard=%d", enableKeyboard ? 1 : 0);
    M5.begin();
    cardputer_audio::configureBootSpeaker();
    _enableKeyboard = enableKeyboard;
    if (enableKeyboard) {
        Keyboard.begin();
    }
    BOOT_LOG("HW", "M5_CARDPUTER::begin bool done");
}

void M5_CARDPUTER::begin(m5::M5Unified::config_t cfg, bool enableKeyboard)
{
    BOOT_LOG("HW", "M5_CARDPUTER::begin cfg keyboard=%d", enableKeyboard ? 1 : 0);
    M5.begin(cfg);
    BOOT_LOG("HW", "M5.begin cfg done");
    cardputer_audio::configureBootSpeaker();
    _enableKeyboard = enableKeyboard;
    if (enableKeyboard) {
        Keyboard.begin();
    }
    BOOT_LOG("HW", "M5_CARDPUTER::begin cfg done");
}

void M5_CARDPUTER::update(void)
{
    M5.update();
    if (_enableKeyboard) {
        Keyboard.updateKeyList();
        Keyboard.updateKeysState();
    }
}
