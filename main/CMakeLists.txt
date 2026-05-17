get_filename_component(APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
set(SRC_ROOT "${APP_ROOT}/src")
set(LIB_ROOT "${APP_ROOT}/lib")

file(GLOB_RECURSE APP_SRCS
    "${SRC_ROOT}/*.c"
    "${SRC_ROOT}/*.cpp"
    "${LIB_ROOT}/arduino-nofrendo/src/*.c"
    "${LIB_ROOT}/M5Cardputer/src/*.cpp"
)

list(FILTER APP_SRCS EXCLUDE REGEX ".*/examples/.*")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/tests/.*")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/docs/.*")

file(GLOB_RECURSE APP_HEADERS
    "${SRC_ROOT}/*.h"
    "${SRC_ROOT}/*.hpp"
    "${SRC_ROOT}/*.hxx"
    "${LIB_ROOT}/arduino-nofrendo/src/*.h"
    "${LIB_ROOT}/M5Cardputer/src/*.h"
    "${LIB_ROOT}/M5Cardputer/src/*.hpp"
)

set(APP_INCLUDE_DIRS "${SRC_ROOT}" "${LIB_ROOT}/arduino-nofrendo/src" "${LIB_ROOT}/M5Cardputer/src")
foreach(header ${APP_HEADERS})
    get_filename_component(header_dir "${header}" DIRECTORY)
    list(APPEND APP_INCLUDE_DIRS "${header_dir}")
endforeach()
list(REMOVE_DUPLICATES APP_INCLUDE_DIRS)
list(FILTER APP_INCLUDE_DIRS EXCLUDE REGEX ".*/compat/msvc$")

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS ${APP_INCLUDE_DIRS}
    REQUIRES
        app_update
        driver
        esp_partition
        esp_timer
        esp_system
        fatfs
        freertos
        heap
        nvs_flash
        sdmmc
        spi_flash
        spiffs
        vfs
        M5GFX
        M5Unified
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    USE_CZ80=1
    CZ80=1
    USE_SDL=1
    DISABLE_WATCHDOGS
    NGP_USE_THREADED_COLORMAPPING
    NGP_ONLY_RENDER_VISIBLE_LINES
    NGP_OPTIMIZATION_16BIT_READ
    GNW_TARGET_MARIO=0
    GNW_TARGET_ZELDA=0
    WS_SAVE_ENABLED=1
    WS_NO_SPLASH_SCREEN=1
    WS_CPU_SAFE_POLL_WAIT_FASTPATH=1
    WS_AUDIO_POLL_MS=4
    NO_ZERO_LUT
    SNES_NO_SOUND
    SNES_NO_APU
    SNES_NO_BYTE2000
    SNES_NO_THREADED_INPUT
    SNES_NO_THREADED_SAVE
    SNES_LOGS=1
    BOOT_DIAG_LOGS=1
    CORE_DEBUG_LEVEL=0
    LOG_LOCAL_LEVEL=ESP_LOG_WARN
    REMOVE_PRINTF
    SOUND_SUPPORT=1
    JUMP_TABLES_IN_TEXT_SECTION=1
    A7800_NO_DATABASE
    LSB_FIRST
    BPP8
    BPS16
    NO_MIDI
    NO_FLOPPY
    NO_SHA1
    NO_MCF
    NO_I8251
    CPC_LSB_FIRST
    LESS_MULTS
    CPC_NODEBUGGER
    GX4000_TRIM_NONPLUS
    GX4000_CART_ONLY
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -ffunction-sections
    -fdata-sections
    -mlongcalls
    -fmerge-all-constants
    -fno-tree-slp-vectorize
    -fomit-frame-pointer
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -Wno-stack-protector
    -Wno-error=unused-function
    -Wno-error=unused-variable
    -Wno-error=deprecated-declarations
    -Wno-error=address
    -Wno-error=array-bounds
    -Wno-error=char-subscripts
    -Wno-error=comment
    -Wno-error=dangling-else
    -Wno-error=format
    -Wno-error=format-truncation
    -Wno-error=implicit-fallthrough
    -Wno-error=maybe-uninitialized
    -Wno-error=misleading-indentation
    -Wno-error=pointer-sign
    -Wno-error=stringop-overflow
    -Wno-error=unused-const-variable
    -Wno-error=unused-label
    -Wno-error=unused-value
    -Wno-error=zero-length-bounds
    -Wno-unused-parameter
    -Wno-sign-compare
    "-include${SRC_ROOT}/compat/arduino_compat.h"
    "-include${SRC_ROOT}/share/emu_log.h"
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

idf_build_set_property(LINK_OPTIONS "-Wl,--gc-sections" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--cref" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--print-memory-usage" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=bmp_create" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=bmp_destroy" APPEND)
