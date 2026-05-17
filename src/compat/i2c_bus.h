#pragma once

#include <stddef.h>
#include <stdint.h>

namespace compat {

bool i2c_port_a_begin(int sda, int scl, uint32_t frequency_hz);
void i2c_port_a_end();
bool i2c_port_a_probe(uint8_t address);
bool i2c_port_a_read(uint8_t address, uint8_t* data, size_t len);
bool i2c_port_a_read_register(uint8_t address, uint8_t reg, uint8_t* data, size_t len);

}
