get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)

set(M5UNIFIED_ROOT "${PROJECT_ROOT}/.pio/libdeps/cardputer_adv_idf/M5Unified")
if(NOT EXISTS "${M5UNIFIED_ROOT}/src")
    set(M5UNIFIED_ROOT "${PROJECT_ROOT}/.pio/libdeps/m5stack-stamps3/M5Unified")
endif()
if(NOT EXISTS "${M5UNIFIED_ROOT}/src")
    message(FATAL_ERROR "M5Unified sources not found. Run PlatformIO once to install lib_deps.")
endif()

file(GLOB M5UNIFIED_SRCS
    "${M5UNIFIED_ROOT}/src/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/imu/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/led/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/power/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/rtc/*.cpp"
)

idf_component_register(
    SRCS ${M5UNIFIED_SRCS}
    INCLUDE_DIRS "${M5UNIFIED_ROOT}/src"
    REQUIRES M5GFX esp_adc driver
)
