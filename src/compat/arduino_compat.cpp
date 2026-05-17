#include "compat/arduino_compat.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

extern "C" uint32_t micros(void)
{
    return (uint32_t)esp_timer_get_time();
}

extern "C" void delay(uint32_t ms)
{
    if (ms == 0) {
        yield();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

extern "C" void delayMicroseconds(uint32_t us)
{
    esp_rom_delay_us(us);
}

extern "C" void yield(void)
{
    taskYIELD();
}
