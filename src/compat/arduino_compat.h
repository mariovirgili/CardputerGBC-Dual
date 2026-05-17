#pragma once

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef PS
#undef PS
#endif
#ifdef BIT8
#undef BIT8
#endif
#ifdef BIT16
#undef BIT16
#endif

#ifdef __cplusplus
extern "C" {
#endif

uint32_t millis(void);
uint32_t micros(void);
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield(void);

#ifdef __cplusplus
}
#endif

#ifndef ets_delay_us
#define ets_delay_us esp_rom_delay_us
#endif

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef LOW
#define LOW 0
#endif
#ifndef HIGH
#define HIGH 1
#endif

#ifndef INPUT
#define INPUT 0x01
#endif
#ifndef OUTPUT
#define OUTPUT 0x03
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 0x05
#endif

static inline void pinMode(int pin, int mode)
{
    gpio_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.intr_type = GPIO_INTR_DISABLE;

    if (mode == OUTPUT) {
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    } else {
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = (mode == INPUT_PULLUP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    }

    gpio_config(&cfg);
}

static inline void digitalWrite(int pin, int value)
{
    gpio_set_level((gpio_num_t)pin, value ? 1 : 0);
}

static inline int digitalRead(int pin)
{
    return gpio_get_level((gpio_num_t)pin);
}
