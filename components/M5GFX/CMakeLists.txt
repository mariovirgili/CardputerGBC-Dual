get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)

set(M5GFX_ROOT "${PROJECT_ROOT}/.pio/libdeps/cardputer_adv_idf/M5GFX")
if(NOT EXISTS "${M5GFX_ROOT}/src")
    set(M5GFX_ROOT "${PROJECT_ROOT}/.pio/libdeps/m5stack-stamps3/M5GFX")
endif()
if(NOT EXISTS "${M5GFX_ROOT}/src")
    message(FATAL_ERROR "M5GFX sources not found. Run PlatformIO once to install lib_deps.")
endif()

if(DEFINED ENV{PIOENV})
    set(M5GFX_GENERATED_ENV "$ENV{PIOENV}")
else()
    set(M5GFX_GENERATED_ENV "cardputer_adv_idf")
endif()
set(M5GFX_GENERATED_DIR "${PROJECT_ROOT}/.pio/build/${M5GFX_GENERATED_ENV}/generated/M5GFX")
file(MAKE_DIRECTORY "${M5GFX_GENERATED_DIR}")

set(M5GFX_SRCS
    "${M5GFX_ROOT}/src/M5GFX.cpp"
    "${M5GFX_ROOT}/src/lgfx/utility/lgfx_miniz.c"
    "${M5GFX_ROOT}/src/lgfx/utility/lgfx_pngle.c"
    "${M5GFX_ROOT}/src/lgfx/utility/lgfx_qoi.c"
    "${M5GFX_ROOT}/src/lgfx/utility/lgfx_qrcode.c"
    "${M5GFX_ROOT}/src/lgfx/utility/lgfx_tjpgd.c"
    "${M5GFX_ROOT}/src/lgfx/v1/LGFXBase.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/lgfx_fonts.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/misc/common_function.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/misc/pixelcopy.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/panel/Panel_Device.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/panel/Panel_LCD.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32/Bus_SPI.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32/Light_PWM.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32/common.cpp"
)

set(M5GFX_COMMON_SRC "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32/common.cpp")
set(M5GFX_COMMON_PATCHED "${M5GFX_GENERATED_DIR}/m5gfx_esp32_common_legacy_i2c.cpp")
file(READ "${M5GFX_COMMON_SRC}" M5GFX_COMMON_CONTENT)

# M5Unified still uses the ESP-IDF legacy I2C driver. LovyanGFX 1.2.x
# auto-selects the new IDF5 driver_ng path when driver/i2c_master.h exists,
# and IDF aborts at runtime if both driver families are initialized. Build
# LGFX's ESP32 platform file with the legacy path so the whole firmware uses
# one I2C driver family.
string(REPLACE
    "__has_include(<driver/i2c_master.h>)"
    "0 && __has_include(<driver/i2c_master.h>)"
    M5GFX_COMMON_CONTENT
    "${M5GFX_COMMON_CONTENT}"
)
file(WRITE "${M5GFX_COMMON_PATCHED}" "${M5GFX_COMMON_CONTENT}")
list(FILTER M5GFX_SRCS EXCLUDE REGEX ".*/lgfx/v1/platforms/esp32/common\\.cpp$")
list(APPEND M5GFX_SRCS "${M5GFX_COMMON_PATCHED}")

set(M5GFX_MAIN_SRC "${M5GFX_ROOT}/src/M5GFX.cpp")
set(M5GFX_MAIN_PATCHED "${M5GFX_GENERATED_DIR}/m5gfx_cardputer_adv.cpp")
file(READ "${M5GFX_MAIN_SRC}" M5GFX_MAIN_CONTENT)

# The stock M5GFX autodetect path carries detection code for every M5Stack
# board. This firmware targets only Cardputer ADV, so keep the same ST7789
# setup but drop the unused panel/touch branches from the linked binary.
set(M5GFX_AUTODETECT_BEGIN "  board_t M5GFX::autodetect(bool use_reset, board_t board)\n  {")
set(M5GFX_AUTODETECT_END "\n#else\n\n  bool M5GFX::init_impl")
string(FIND "${M5GFX_MAIN_CONTENT}" "${M5GFX_AUTODETECT_BEGIN}" M5GFX_AUTODETECT_BEGIN_POS)
string(FIND "${M5GFX_MAIN_CONTENT}" "${M5GFX_AUTODETECT_END}" M5GFX_AUTODETECT_END_POS)
if(M5GFX_AUTODETECT_BEGIN_POS EQUAL -1 OR M5GFX_AUTODETECT_END_POS EQUAL -1)
    message(FATAL_ERROR "Could not patch M5GFX autodetect for Cardputer ADV-only build")
endif()
string(SUBSTRING "${M5GFX_MAIN_CONTENT}" 0 ${M5GFX_AUTODETECT_BEGIN_POS} M5GFX_MAIN_PREFIX)
string(SUBSTRING "${M5GFX_MAIN_CONTENT}" ${M5GFX_AUTODETECT_END_POS} -1 M5GFX_MAIN_SUFFIX)
set(M5GFX_CARDPUTER_AUTODETECT [=[
  board_t M5GFX::autodetect(bool use_reset, board_t board)
  {
    (void)board;

    _pin_reset(GPIO_NUM_33, use_reset);

    auto bus_spi = new Bus_SPI();
    _bus_last.reset(bus_spi);
    panel(nullptr);

    auto bus_cfg = bus_spi->config();
    bus_cfg.spi_host = SPI3_HOST;
    bus_cfg.freq_write = 40000000;
    bus_cfg.freq_read = 16000000;
    bus_cfg.pin_mosi = GPIO_NUM_35;
    bus_cfg.pin_miso = (gpio_num_t)-1;
    bus_cfg.pin_sclk = GPIO_NUM_36;
    bus_cfg.pin_dc = GPIO_NUM_34;
    bus_cfg.spi_mode = 0;
    bus_cfg.spi_3wire = true;
    bus_cfg.use_lock = true;
    bus_spi->config(bus_cfg);
    bus_spi->init();

    auto p = new Panel_ST7789();
    p->bus(bus_spi);
    auto cfg = p->config();
    cfg.pin_cs = GPIO_NUM_37;
    cfg.pin_rst = GPIO_NUM_33;
    cfg.panel_width = 135;
    cfg.panel_height = 240;
    cfg.offset_x = 52;
    cfg.offset_y = 40;
    cfg.offset_rotation = 0;
    cfg.readable = true;
    cfg.invert = true;
    p->config(cfg);
    p->setRotation(1);

    _panel_last.reset(p);
    _set_pwm_backlight(GPIO_NUM_38, 7, 256, false, 16);
    panel(_panel_last.get());
    return board_t::board_M5CardputerADV;
  }
]=]
)
set(M5GFX_MAIN_CONTENT "${M5GFX_MAIN_PREFIX}${M5GFX_CARDPUTER_AUTODETECT}${M5GFX_MAIN_SUFFIX}")
file(WRITE "${M5GFX_MAIN_PATCHED}" "${M5GFX_MAIN_CONTENT}")
list(FILTER M5GFX_SRCS EXCLUDE REGEX ".*/src/M5GFX\\.cpp$")
list(APPEND M5GFX_SRCS "${M5GFX_MAIN_PATCHED}")

idf_component_register(
    SRCS ${M5GFX_SRCS}
    INCLUDE_DIRS
        "${M5GFX_ROOT}/src"
        "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32"
    REQUIRES nvs_flash efuse driver esp_timer esp_lcd esp_mm
)
