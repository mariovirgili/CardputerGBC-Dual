#include "compat/i2c_bus.h"

#include "driver/i2c.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace {

// M5Unified uses I2C_NUM_1 for the Cardputer internal bus (TCA8418/PMU).
// Port A / external joystick is on the external bus, I2C_NUM_0.
constexpr i2c_port_t kPort = I2C_NUM_0;
bool s_installed = false;
int s_sda = -1;
int s_scl = -1;
uint32_t s_frequency = 0;

bool ensure_port(int sda, int scl, uint32_t frequency_hz)
{
    if (s_installed && s_sda == sda && s_scl == scl && s_frequency == frequency_hz) {
        return true;
    }

    if (s_installed) {
        i2c_driver_delete(kPort);
        s_installed = false;
    }

    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = sda;
    conf.scl_io_num = scl;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = frequency_hz;

    if (i2c_param_config(kPort, &conf) != ESP_OK) {
        return false;
    }
    if (i2c_driver_install(kPort, conf.mode, 0, 0, 0) != ESP_OK) {
        return false;
    }

    s_installed = true;
    s_sda = sda;
    s_scl = scl;
    s_frequency = frequency_hz;
    return true;
}

}  // namespace

namespace compat {

bool i2c_port_a_begin(int sda, int scl, uint32_t frequency_hz)
{
    return ensure_port(sda, scl, frequency_hz);
}

void i2c_port_a_end()
{
    if (s_installed) {
        i2c_driver_delete(kPort);
        s_installed = false;
        s_sda = -1;
        s_scl = -1;
        s_frequency = 0;
    }
}

bool i2c_port_a_probe(uint8_t address)
{
    if (!s_installed) {
        return false;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        return false;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    const esp_err_t err = i2c_master_cmd_begin(kPort, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

bool i2c_port_a_read(uint8_t address, uint8_t* data, size_t len)
{
    if (!s_installed || !data || len == 0) {
        return false;
    }
    return i2c_master_read_from_device(kPort, address, data, len, pdMS_TO_TICKS(50)) == ESP_OK;
}

bool i2c_port_a_read_register(uint8_t address, uint8_t reg, uint8_t* data, size_t len)
{
    if (!s_installed || !data || len == 0) {
        return false;
    }
    return i2c_master_write_read_device(kPort, address, &reg, 1, data, len, pdMS_TO_TICKS(50)) == ESP_OK;
}

}  // namespace compat
