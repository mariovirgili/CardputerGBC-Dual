get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)

set(M5GFX_ROOT "${PROJECT_ROOT}/.pio/libdeps/cardputer_adv_idf/M5GFX")
if(NOT EXISTS "${M5GFX_ROOT}/src")
    set(M5GFX_ROOT "${PROJECT_ROOT}/.pio/libdeps/m5stack-stamps3/M5GFX")
endif()
if(NOT EXISTS "${M5GFX_ROOT}/src")
    message(FATAL_ERROR "M5GFX sources not found. Run PlatformIO once to install lib_deps.")
endif()

file(GLOB M5GFX_SRCS
    "${M5GFX_ROOT}/src/*.cpp"
    "${M5GFX_ROOT}/src/lgfx/Fonts/efont/*.c"
    "${M5GFX_ROOT}/src/lgfx/Fonts/IPA/*.c"
    "${M5GFX_ROOT}/src/lgfx/Fonts/lvgl/*.c"
    "${M5GFX_ROOT}/src/lgfx/utility/*.c"
    "${M5GFX_ROOT}/src/lgfx/v1/*.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/lv_font/*.c"
    "${M5GFX_ROOT}/src/lgfx/v1/misc/*.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/panel/*.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32/*.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32c3/*.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32s3/*.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32p4/*.cpp"
    "${M5GFX_ROOT}/src/lgfx/v1/touch/*.cpp"
)

set(M5GFX_COMMON_SRC "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32/common.cpp")
set(M5GFX_COMMON_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/m5gfx_esp32_common_legacy_i2c.cpp")
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

idf_component_register(
    SRCS ${M5GFX_SRCS}
    INCLUDE_DIRS
        "${M5GFX_ROOT}/src"
        "${M5GFX_ROOT}/src/lgfx/v1/platforms/esp32"
    REQUIRES nvs_flash efuse driver esp_timer esp_lcd esp_mm
)
