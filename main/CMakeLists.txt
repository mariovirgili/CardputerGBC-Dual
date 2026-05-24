get_filename_component(APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
set(SRC_ROOT "${APP_ROOT}/src")
set(LIB_ROOT "${APP_ROOT}/lib")

# Core inclusion switches. Disabled cores keep their source code in the tree,
# but are not compiled, advertised, or launchable.
set(ENABLE_SNES_CORE OFF)

file(GLOB_RECURSE APP_SRCS
    "${SRC_ROOT}/*.c"
    "${SRC_ROOT}/*.cpp"
    "${LIB_ROOT}/arduino-nofrendo/src/*.c"
    "${LIB_ROOT}/M5Cardputer/src/*.cpp"
)

list(FILTER APP_SRCS EXCLUDE REGEX ".*/examples/.*")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/tests/.*")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/docs/.*")
if(NOT ENABLE_SNES_CORE)
    list(FILTER APP_SRCS EXCLUDE REGEX ".*/src/snes/.*")
endif()

# Local logging switches. Keep these here, not in platformio.ini, so toggling
# a diagnostic path only rebuilds the source files that actually use it.

# ---==  LOG SECTION:  ==---
# --= Global =--
# master switch; enables generic emulator logs.
set(ENABLE_EMU_LOGS ON)
set(ENABLE_BOOT_DIAG_LOGS ON)
# startup heap checkpoints, requires ENABLE_EMU_LOGS.
set(ENABLE_HEAP_LOGS ON)
set(ENABLE_AUDIO_LOGS OFF)
# extra perf/detail logs, requires ENABLE_EMU_LOGS.
set(ENABLE_BENCHMARK_LOGS OFF)
# allow raw printf while emu_log.h is forced-included.
set(ENABLE_EMU_ALLOW_RAW_PRINTF OFF)

# GWENESIS: Mega Drive render/audio detail logs, requires ENABLE_EMU_LOGS.
set(ENABLE_MD_LOGS ON)
set(ENABLE_MD_AUDIO_LOGS OFF)
set(ENABLE_MD_RENDER_LOGS OFF)

# FMSX: fMSX detail logs, requires ENABLE_EMU_LOGS.
set(ENABLE_MSX_LOGS ON)

# SNES: SNES detail logs, requires ENABLE_EMU_LOGS.
set(ENABLE_SNES_LOGS ON)
# SNES: deep CPU/render benchmark and targeted PPU probes.
set(ENABLE_SNES_DEEP_BENCH OFF)

# NES: diagnostic logs, requires ENABLE_EMU_LOGS.
set(ENABLE_NES_DIAG_LOGS OFF)
set(ENABLE_NES_BENCHMARK_LOGS ON)

# Oswan: WonderSwan detail logs, requires ENABLE_EMU_LOGS.
set(ENABLE_WS_LOGS OFF)
set(ENABLE_WS_BENCHMARK_LOGS OFF)
# Oswan: temporary WS/WSC model and COLCTL diagnostics.
set(ENABLE_WS_MODEL_LOGS OFF)
# Oswan: temporary lightweight WS renderer section profiling.
set(ENABLE_WS_RENDER_PROFILE OFF)
# Oswan: temporary log hot NEC opcodes for IRAM slimming.
set(ENABLE_WS_CPU_PROFILE OFF)
# Oswan: temporary log hot backward branch PCs.
set(ENABLE_WS_CPU_BRANCH_PROFILE OFF)
# Oswan: experimental; reduced decode calls but hurt frame time/heap.

# Race (NGP): Neo Geo Pocket trace logs, requires ENABLE_EMU_LOGS.
set(ENABLE_NGP_TRACE_LOGS OFF)

# GX4000/Coleco-SMS: Coleco/SMS detail logs, requires ENABLE_EMU_LOGS.
set(ENABLE_COLECO_DEBUG_LOGS OFF)

# ProSystem (Atari 7800): allocation/core diagnostics, requires ENABLE_EMU_LOGS.
set(ENABLE_A7800_LOGS OFF)

# ---==  CORE COMPILE OPTIONS SECTION:  ==---
# Core compile-definition switches. Defaults mirror the current firmware build;
# flip one to OFF here to remove its define without touching platformio.ini.

# Global
set(ENABLE_CORE_DEBUG_LEVEL_DEFINE ON)
set(ENABLE_LOG_LOCAL_LEVEL_DEFINE ON)
set(ENABLE_REMOVE_PRINTF_DEFINE ON)
# Global speed/heap profile for RAM-expensive optimizations.
# ON keeps the fast IRAM/inline paths; set OFF to recover heap/IRAM.
set(TURN_OFF_IF_HEAP_LOW ON)
# Cores listed here close the SD card after ROM/save load and remount it only
# for explicit save work. Each core still owns its SRAM/runtime constraints.
set(SD_OFF_DURING_GAMEPLAY_CORES SNES WS)

# Race (NGP)
set(ENABLE_NGP_USE_CZ80 ON)
set(ENABLE_NGP_CZ80 ON)
set(ENABLE_NGP_USE_SDL ON)
set(ENABLE_NGP_DISABLE_WATCHDOGS ON)
# Use a separate thread for color mapping to improve performance.
set(ENABLE_NGP_THREADED_COLORMAPPING OFF)
# Disables rendering of lines that are not visible due to scaling.
set(ENABLE_NGP_ONLY_RENDER_VISIBLE_LINES ON)
# 16-bit reads with alignment check for native bus transactions.
set(ENABLE_NGP_OPTIMIZATION_16BIT_READ ON)

# GWENESIS
set(ENABLE_GNW_TARGET_MARIO_DEFINE ON)
set(ENABLE_GNW_TARGET_ZELDA_DEFINE ON)

# Oswan
set(ENABLE_WS_SAVE ON)
set(ENABLE_WS_NO_SPLASH_SCREEN ON)
# guarded NEC polling wait-loop fast paths.
set(ENABLE_WS_CPU_SAFE_POLL_WAIT_FASTPATH ON)
# test only; legacy M5 speaker path refills/checks queue twice as often.
# Direct I2S mode ignores this and drives audio as fast as DMA accepts it.
set(ENABLE_WS_AUDIO_POLL_MS ON)
# test only; bypass M5Unified playRaw queue and write WS audio directly to I2S.
set(ENABLE_WS_DIRECT_I2S_AUDIO ON)
set(ENABLE_WS_TILE_ROW_CACHE OFF)
set(WS_TILE_ROW_CACHE_ENTRIES 2048)
set(ENABLE_WS_PPU_IRAM ON)
set(ENABLE_WS_APU_IRAM ON)
set(ENABLE_WS_CORE_IRAM ON)
set(ENABLE_WS_RENDER_INLINE_FASTPATHS ON)
set(ENABLE_WS_RENDER_TILE_SPLIT ON)
set(WS_TURN_OFF_IF_HEAP_LOW ${TURN_OFF_IF_HEAP_LOW})
# test only; keep current WS task layout explicit for scheduler experiments.
set(WS_DISPLAY_TASK_CORE "0")
set(WS_DISPLAY_TASK_PRIORITY "5")
set(WS_AUDIO_TASK_CORE "0")
set(WS_AUDIO_TASK_PRIORITY "6")
# test only; shorter 4 ms blocks reduce time-to-speaker and underflow recovery.
set(WS_AUDIO_PERIOD_MS "4")
# test only; smaller DMA frames keep the direct I2S pipeline more responsive.
set(WS_AUDIO_DMA_LEN "256")
# test only; deeper DMA queue adds slack for heavy render spikes.
set(WS_AUDIO_DMA_COUNT "8")

# SNES
set(ENABLE_SNES_NO_ZERO_LUT ON)
# The decoded tile cache is disabled; skip its stale/valid flags too.
set(ENABLE_SNES_NO_TILE_CACHE_FLAGS ON)
# Disable sound support.
set(ENABLE_SNES_NO_SOUND ON)
set(ENABLE_SNES_NO_APU ON)
set(ENABLE_SNES_NO_BYTE2000 ON)
set(ENABLE_SNES_NO_THREADED_DISPLAY ON)
set(ENABLE_SNES_NO_THREADED_INPUT ON)
set(ENABLE_SNES_NO_THREADED_SAVE ON)
# XIP ROMs are flash-mapped read-only; never patch ROM bytes in-place.
set(ENABLE_SNES_NO_ROM_PATCHES ON)
set(ENABLE_SNES_NO_SPEEDHACKS ON)
# ESP32-S3 is little-endian and supports unaligned word loads.
set(ENABLE_SNES_FAST_LSB_WORD_ACCESS ON)
# SNES experiment: reduce code bloat/ICache pressure from loop unrolling.
set(ENABLE_SNES_NO_UNROLL_LOOPS ON)
# SNES experiment: allow faster floating-point math where the core uses it.
set(ENABLE_SNES_FAST_MATH ON)
# SNES experiment: optimize only hot runtime paths with -O3.
set(ENABLE_SNES_HOT_O3 ON)
# SNES: keep pseudo-hires on the same narrow condition used by PocketSNES.
set(ENABLE_SNES_STRICT_PSEUDO_HIRES ON)
# SNES experiment: remove zero-pixel branches in the common 4bpp tile decoder.
set(ENABLE_SNES_BRANCHLESS_CONVERT_TILE_4BPP ON)
# SNES experiment: specialize color add/sub for RGB565.
set(ENABLE_SNES_FAST_RGB565_COLOR_MATH ON)
# SNES experiment: keep get/set wrappers out of IRAM; use Xtensa asm fast paths instead.
set(ENABLE_SNES_GETSET_IRAM OFF)
# SNES experiment: compile CPU opcode and memory dispatch with -Ofast.
set(ENABLE_SNES_CPU_MEM_OFAST ON)
# SNES experiment: start line buffers at normal width and grow for hi-res only.
set(ENABLE_SNES_LAZY_LINE_BUFFERS ON)


# Stella (Atari 2600)
set(ENABLE_A2600_SOUND_SUPPORT ON)
set(ENABLE_A2600_JUMP_TABLES_IN_TEXT_SECTION ON)

# ProSystem (Atari 7800): Drop built-in 7800 MD5 database (~5 KB flash);
# headered ROMs still work.
set(ENABLE_A7800_NO_DATABASE OFF)

# fMSX
set(ENABLE_FMSX_LSB_FIRST ON)
set(ENABLE_FMSX_BPP8 ON)
set(ENABLE_FMSX_BPS16 ON)
# Disable MIDI logging tables (~12KB flash).
set(ENABLE_FMSX_NO_MIDI ON)
# Disable floppy/disk subsystem (ROM-only).
set(ENABLE_FMSX_NO_FLOPPY ON)
# Disable SHA1 ROM fingerprinting (no CARTS.SHA on SD).
set(ENABLE_FMSX_NO_SHA1 ON)
# Disable MCF cheat-file support.
set(ENABLE_FMSX_NO_MCF ON)
# Disable RS232 i8251 runtime support.
set(ENABLE_FMSX_NO_I8251 ON)

# GX4000
# ESP32 is little-endian.
set(ENABLE_GX4000_CPC_LSB_FIRST ON)
# Use pScreenLine-based address calc (no multiply per NOP).
set(ENABLE_GX4000_LESS_MULTS ON)
# Strip out debugger UI code.
set(ENABLE_GX4000_CPC_NODEBUGGER ON)
# Trim legacy CPC/KC hardware paths (GX4000 is CPC+ only).
set(ENABLE_GX4000_TRIM_NONPLUS ON)
# Strip cassette/floppy/printer paths for cart-only GX4000.
set(ENABLE_GX4000_CART_ONLY ON)

# Selective LTO. Start with isolated/stable cores so benchmarks can compare
# flash size, FPS, and audio before extending this to more fragile cores.
# Use fat LTO objects here: pure slim LTO currently leaves the ESP-IDF/PlatformIO
# Windows linker asking for the LTO plugin and drops the core symbols at link.
set(ENABLE_GX4000_LTO ON)
set(ENABLE_SMS_LTO ON)
set(ENABLE_NES_LTO ON)
set(ENABLE_A2600_LTO ON)
set(ENABLE_A7800_LTO ON)
set(ENABLE_PCE_LTO OFF)
set(ENABLE_GBC_LTO ON)
set(ENABLE_LYNX_LTO ON)
set(ENABLE_MSX_LTO ON)
set(ENABLE_WS_LTO ON)
set(ENABLE_MD_LTO ON)
set(ENABLE_NGP_LTO ON)
set(ENABLE_SNES_LTO ON)
set(ENABLE_EXPLICIT_LTO_PLUGIN OFF)

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
if(NOT ENABLE_SNES_CORE)
    list(FILTER APP_INCLUDE_DIRS EXCLUDE REGEX ".*/src/snes($|/.*)")
endif()

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS ${APP_INCLUDE_DIRS}
    REQUIRES
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

function(add_private_define_if enable_var definition)
    if(${enable_var})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${definition})
    endif()
endfunction()

add_private_define_if(ENABLE_SNES_CORE SNES_CORE_ENABLED=1)

function(add_sd_off_define core_name definition)
    list(FIND SD_OFF_DURING_GAMEPLAY_CORES "${core_name}" _sd_off_index)
    if(NOT _sd_off_index EQUAL -1)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${definition})
    endif()
endfunction()

add_sd_off_define(WS WS_SD_OFF_DURING_GAMEPLAY=1)
if(ENABLE_SNES_CORE)
    add_sd_off_define(SNES SNES_SD_OFF_DURING_GAMEPLAY=1)
endif()

# Race (NGP)
add_private_define_if(ENABLE_NGP_USE_CZ80 USE_CZ80=1)
add_private_define_if(ENABLE_NGP_CZ80 CZ80=1)
add_private_define_if(ENABLE_NGP_USE_SDL USE_SDL=1)
add_private_define_if(ENABLE_NGP_DISABLE_WATCHDOGS DISABLE_WATCHDOGS)
# Race (NGP): Use a separate thread for color mapping to improve performance.
add_private_define_if(ENABLE_NGP_THREADED_COLORMAPPING NGP_USE_THREADED_COLORMAPPING)
# Race (NGP): Disables rendering of lines that are not visible due to scaling.
add_private_define_if(ENABLE_NGP_ONLY_RENDER_VISIBLE_LINES NGP_ONLY_RENDER_VISIBLE_LINES)
# Race (NGP): 16-bit reads with alignment check for native bus transactions.
add_private_define_if(ENABLE_NGP_OPTIMIZATION_16BIT_READ NGP_OPTIMIZATION_16BIT_READ)

# GWENESIS
add_private_define_if(ENABLE_GNW_TARGET_MARIO_DEFINE GNW_TARGET_MARIO=0)
add_private_define_if(ENABLE_GNW_TARGET_ZELDA_DEFINE GNW_TARGET_ZELDA=0)

# Oswan
if(NOT WS_TURN_OFF_IF_HEAP_LOW)
    set(ENABLE_WS_TILE_ROW_CACHE OFF)
    set(ENABLE_WS_RENDER_INLINE_FASTPATHS OFF)
    set(ENABLE_WS_RENDER_TILE_SPLIT OFF)
endif()

add_private_define_if(ENABLE_WS_SAVE WS_SAVE_ENABLED=1)
add_private_define_if(ENABLE_WS_NO_SPLASH_SCREEN WS_NO_SPLASH_SCREEN=1)
add_private_define_if(ENABLE_WS_PPU_IRAM WS_PPU_IRAM=1)
# Oswan: guarded NEC polling wait-loop fast paths.
add_private_define_if(ENABLE_WS_CPU_SAFE_POLL_WAIT_FASTPATH WS_CPU_SAFE_POLL_WAIT_FASTPATH=1)
# Oswan: test only; keep 8 ms blocks, refill/check speaker queue twice as often.
add_private_define_if(ENABLE_WS_AUDIO_POLL_MS WS_AUDIO_POLL_MS=4)
add_private_define_if(ENABLE_WS_DIRECT_I2S_AUDIO WS_DIRECT_I2S_AUDIO=1)
add_private_define_if(ENABLE_WS_RENDER_INLINE_FASTPATHS WS_RENDER_INLINE_FASTPATHS=1)
add_private_define_if(ENABLE_WS_RENDER_TILE_SPLIT WS_RENDER_TILE_SPLIT=1)

if(ENABLE_SNES_CORE)
    # SNES
    add_private_define_if(ENABLE_SNES_NO_ZERO_LUT NO_ZERO_LUT)
    add_private_define_if(ENABLE_SNES_NO_TILE_CACHE_FLAGS SNES_NO_TILE_CACHE_FLAGS)
    # SNES: Disable sound support.
    add_private_define_if(ENABLE_SNES_NO_SOUND SNES_NO_SOUND)
    add_private_define_if(ENABLE_SNES_NO_APU SNES_NO_APU)
    add_private_define_if(ENABLE_SNES_NO_BYTE2000 SNES_NO_BYTE2000)
    add_private_define_if(ENABLE_SNES_NO_THREADED_DISPLAY SNES_NO_THREADED_DISPLAY)
    add_private_define_if(ENABLE_SNES_NO_THREADED_DISPLAY SNES_DISPLAY_ON_MAIN)
    add_private_define_if(ENABLE_SNES_NO_THREADED_INPUT SNES_NO_THREADED_INPUT)
    add_private_define_if(ENABLE_SNES_NO_THREADED_SAVE SNES_NO_THREADED_SAVE)
    add_private_define_if(ENABLE_SNES_NO_ROM_PATCHES SNES_NO_ROM_PATCHES)
    add_private_define_if(ENABLE_SNES_NO_SPEEDHACKS NO_SPEEDHACKS)
    add_private_define_if(ENABLE_SNES_FAST_LSB_WORD_ACCESS FAST_LSB_WORD_ACCESS)
    add_private_define_if(ENABLE_SNES_STRICT_PSEUDO_HIRES SNES_STRICT_PSEUDO_HIRES)
    add_private_define_if(ENABLE_SNES_BRANCHLESS_CONVERT_TILE_4BPP SNES_BRANCHLESS_CONVERT_TILE_4BPP)
    add_private_define_if(ENABLE_SNES_FAST_RGB565_COLOR_MATH SNES_FAST_RGB565_COLOR_MATH)
    add_private_define_if(ENABLE_SNES_GETSET_IRAM SNES_GETSET_IRAM)
    add_private_define_if(ENABLE_SNES_LAZY_LINE_BUFFERS SNES_LAZY_LINE_BUFFERS)
endif()

# Global
add_private_define_if(ENABLE_CORE_DEBUG_LEVEL_DEFINE CORE_DEBUG_LEVEL=0)
add_private_define_if(ENABLE_LOG_LOCAL_LEVEL_DEFINE LOG_LOCAL_LEVEL=ESP_LOG_WARN)
add_private_define_if(ENABLE_REMOVE_PRINTF_DEFINE REMOVE_PRINTF)

# Stella (Atari 2600)
add_private_define_if(ENABLE_A2600_SOUND_SUPPORT SOUND_SUPPORT=1)
add_private_define_if(ENABLE_A2600_JUMP_TABLES_IN_TEXT_SECTION JUMP_TABLES_IN_TEXT_SECTION=1)

# ProSystem (Atari 7800): Drop built-in 7800 MD5 database (~5 KB flash);
# headered ROMs still work.
add_private_define_if(ENABLE_A7800_NO_DATABASE A7800_NO_DATABASE)

# fMSX
add_private_define_if(ENABLE_FMSX_LSB_FIRST LSB_FIRST)
add_private_define_if(ENABLE_FMSX_BPP8 BPP8)
add_private_define_if(ENABLE_FMSX_BPS16 BPS16)
# fMSX: Disable MIDI logging tables (~12KB flash).
add_private_define_if(ENABLE_FMSX_NO_MIDI NO_MIDI)
# fMSX: Disable floppy/disk subsystem (ROM-only).
add_private_define_if(ENABLE_FMSX_NO_FLOPPY NO_FLOPPY)
# fMSX: Disable SHA1 ROM fingerprinting (no CARTS.SHA on SD).
add_private_define_if(ENABLE_FMSX_NO_SHA1 NO_SHA1)
# fMSX: Disable MCF cheat-file support.
add_private_define_if(ENABLE_FMSX_NO_MCF NO_MCF)
# fMSX: Disable RS232 i8251 runtime support.
add_private_define_if(ENABLE_FMSX_NO_I8251 NO_I8251)

# GX4000
# GX4000: ESP32 is little-endian.
add_private_define_if(ENABLE_GX4000_CPC_LSB_FIRST CPC_LSB_FIRST)
# GX4000: Use pScreenLine-based address calc (no multiply per NOP).
add_private_define_if(ENABLE_GX4000_LESS_MULTS LESS_MULTS)
# GX4000: Strip out debugger UI code.
add_private_define_if(ENABLE_GX4000_CPC_NODEBUGGER CPC_NODEBUGGER)
# GX4000: Trim legacy CPC/KC hardware paths (GX4000 is CPC+ only).
add_private_define_if(ENABLE_GX4000_TRIM_NONPLUS GX4000_TRIM_NONPLUS)
# GX4000: Strip cassette/floppy/printer paths for cart-only GX4000.
add_private_define_if(ENABLE_GX4000_CART_ONLY GX4000_CART_ONLY)

if(ENABLE_WS_TILE_ROW_CACHE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        # Oswan: experimental; reduced decode calls but hurt frame time/heap.
        WS_TILE_ROW_CACHE=1
        WS_TILE_ROW_CACHE_ENTRIES=${WS_TILE_ROW_CACHE_ENTRIES}
    )
endif()

if(ENABLE_WS_APU_IRAM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_APU_IRAM=1)
endif()

if(ENABLE_WS_CORE_IRAM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_CORE_IRAM=1)
endif()

if(WS_AUDIO_PERIOD_MS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_AUDIO_PERIOD_MS=${WS_AUDIO_PERIOD_MS})
endif()

if(WS_DISPLAY_TASK_CORE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_DISPLAY_TASK_CORE=${WS_DISPLAY_TASK_CORE})
endif()

if(WS_DISPLAY_TASK_PRIORITY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_DISPLAY_TASK_PRIORITY=${WS_DISPLAY_TASK_PRIORITY})
endif()

if(WS_AUDIO_TASK_CORE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_AUDIO_TASK_CORE=${WS_AUDIO_TASK_CORE})
endif()

if(WS_AUDIO_TASK_PRIORITY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_AUDIO_TASK_PRIORITY=${WS_AUDIO_TASK_PRIORITY})
endif()

if(WS_AUDIO_DMA_LEN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_AUDIO_DMA_LEN=${WS_AUDIO_DMA_LEN})
endif()

if(WS_AUDIO_DMA_COUNT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WS_AUDIO_DMA_COUNT=${WS_AUDIO_DMA_COUNT})
endif()

function(add_source_compile_defs)
    set(options)
    set(one_value_args)
    set(multi_value_args DEFINITIONS SOURCES)
    cmake_parse_arguments(SRCDEF "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    foreach(src ${SRCDEF_SOURCES})
        if(EXISTS "${src}")
            set_property(SOURCE "${src}" APPEND PROPERTY COMPILE_DEFINITIONS ${SRCDEF_DEFINITIONS})
        endif()
    endforeach()
endfunction()

function(add_source_compile_opts)
    set(options)
    set(one_value_args)
    set(multi_value_args OPTIONS SOURCES)
    cmake_parse_arguments(SRCOPT "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    foreach(src ${SRCOPT_SOURCES})
        if(EXISTS "${src}")
            set_property(SOURCE "${src}" APPEND PROPERTY COMPILE_OPTIONS ${SRCOPT_OPTIONS})
        endif()
    endforeach()
endfunction()

set(GENERAL_LOG_SRCS
    "${SRC_ROOT}/main.cpp"
    "${SRC_ROOT}/share/input.cpp"
    "${SRC_ROOT}/vfs/partitioner.cpp"
    "${SRC_ROOT}/atari2600/a2600_display.cpp"
    "${SRC_ROOT}/atari2600/run_a2600.cpp"
    "${SRC_ROOT}/atari7800/a7800_input.cpp"
    "${SRC_ROOT}/atari7800/a7800_video.cpp"
    "${SRC_ROOT}/atari7800/run_a7800.cpp"
    "${SRC_ROOT}/gbc/gbc_display.cpp"
    "${SRC_ROOT}/gbc/gbc_input.cpp"
    "${SRC_ROOT}/gbc/gbc_save.cpp"
    "${SRC_ROOT}/gbc/run_gbc.cpp"
    "${SRC_ROOT}/genesis/genesis_display.cpp"
    "${SRC_ROOT}/genesis/genesis_save.cpp"
    "${SRC_ROOT}/genesis/run_genesis.cpp"
    "${SRC_ROOT}/gx4000/gx4000_display.cpp"
    "${SRC_ROOT}/gx4000/gx4000_host.c"
    "${SRC_ROOT}/gx4000/run_gx4000.cpp"
    "${SRC_ROOT}/lynx/lynx_display.cpp"
    "${SRC_ROOT}/lynx/run_lynx.cpp"
    "${SRC_ROOT}/msx/msx_display.cpp"
    "${SRC_ROOT}/msx/msx_host.cpp"
    "${SRC_ROOT}/msx/run_msx.cpp"
    "${SRC_ROOT}/nes/nes_osd.c"
    "${SRC_ROOT}/nes/run_nes.cpp"
    "${SRC_ROOT}/ngp/ngc_display.cpp"
    "${SRC_ROOT}/ngp/ngc_save.cpp"
    "${SRC_ROOT}/ngp/race/graphics.c"
    "${SRC_ROOT}/ngp/race/tlcs900h.c"
    "${SRC_ROOT}/ngp/run_ngp.cpp"
    "${SRC_ROOT}/pce/pce_display.cpp"
    "${SRC_ROOT}/pce/pce-go/pce-go.c"
    "${SRC_ROOT}/pce/run_pce.cpp"
    "${SRC_ROOT}/sms/display.cpp"
    "${SRC_ROOT}/sms/run_sms.cpp"
    "${SRC_ROOT}/sms/save.cpp"
    "${SRC_ROOT}/sms/smsplus/sms.c"
    "${SRC_ROOT}/ws/oswan/WSApu.c"
    "${SRC_ROOT}/ws/oswan/WSFileio.c"
    "${SRC_ROOT}/ws/oswan/cpu/nec.c"
    "${SRC_ROOT}/ws/run_ws.cpp"
    "${SRC_ROOT}/ws/ws_display.cpp"
    "${SRC_ROOT}/ws/ws_save.cpp"
    "${SRC_ROOT}/ws/ws_state.cpp"
    "${LIB_ROOT}/arduino-nofrendo/src/nes/nes.c"
)

set(BOOT_LOG_SRCS
    "${SRC_ROOT}/main.cpp"
    "${SRC_ROOT}/share/input.cpp"
    "${SRC_ROOT}/cardputer/CardputerView.cpp"
)

set(AUDIO_LOG_SRCS
    "${SRC_ROOT}/atari2600/a2600_sound.cpp"
    "${SRC_ROOT}/atari7800/a7800_audio.cpp"
    "${SRC_ROOT}/gbc/gbc_sound.cpp"
    "${SRC_ROOT}/genesis/genesis_sound.cpp"
    "${SRC_ROOT}/gx4000/gx4000_sound.cpp"
    "${SRC_ROOT}/lynx/lynx_sound.cpp"
    "${SRC_ROOT}/msx/msx_audio.cpp"
    "${SRC_ROOT}/nes/nes_sound.cpp"
    "${SRC_ROOT}/ngp/ngc_sound.cpp"
    "${SRC_ROOT}/pce/pce_sound.cpp"
    "${SRC_ROOT}/sms/sound.cpp"
    "${SRC_ROOT}/ws/ws_sound.cpp"
)

set(MD_RENDER_LOG_SRCS
    "${SRC_ROOT}/genesis/genesis_display.cpp"
    "${SRC_ROOT}/genesis/run_genesis.cpp"
    "${SRC_ROOT}/genesis/gwenesis/bus/gwenesis_bus.c"
    "${SRC_ROOT}/genesis/gwenesis/vdp/gwenesis_vdp_mem.c"
    "${SRC_ROOT}/genesis/gwenesis/vdp/gwenesis_vdp_gfx.c"
)

set(MD_AUDIO_LOG_SRCS
    "${SRC_ROOT}/genesis/genesis_sound.cpp"
    "${SRC_ROOT}/genesis/gwenesis/cpus/Z80/ConDebug.c"
    "${SRC_ROOT}/genesis/gwenesis/cpus/Z80/Debug.c"
    "${SRC_ROOT}/genesis/gwenesis/sound/z80inst.c"
    "${SRC_ROOT}/genesis/gwenesis/sound/ym2612.c"
)

set(MSX_LOG_SRCS
    "${SRC_ROOT}/msx/run_msx.cpp"
    "${SRC_ROOT}/msx/fMSX/MSX.c"
)

set(NES_LOG_SRCS
    "${SRC_ROOT}/nes/nes_osd.c"
    "${SRC_ROOT}/nes/run_nes.cpp"
    "${LIB_ROOT}/arduino-nofrendo/src/log.c"
    "${LIB_ROOT}/arduino-nofrendo/src/mappers/map002.c"
    "${LIB_ROOT}/arduino-nofrendo/src/nes/nes.c"
    "${LIB_ROOT}/arduino-nofrendo/src/nes/nes_rom.c"
)

set(WS_LOG_SRCS
    "${SRC_ROOT}/ws/run_ws.cpp"
    "${SRC_ROOT}/ws/ws_display.cpp"
    "${SRC_ROOT}/ws/ws_save.cpp"
    "${SRC_ROOT}/ws/ws_state.cpp"
    "${SRC_ROOT}/ws/oswan/WSFileio.c"
)

set(WS_BENCHMARK_LOG_SRCS
    "${SRC_ROOT}/ws/run_ws.cpp"
    "${SRC_ROOT}/ws/ws_display.cpp"
    "${SRC_ROOT}/ws/ws_sound.cpp"
    "${SRC_ROOT}/ws/oswan/WS.c"
    "${SRC_ROOT}/ws/oswan/WSRender.c"
    "${SRC_ROOT}/ws/oswan/cpu/nec.c"
)

set(NGP_TRACE_LOG_SRCS
    "${SRC_ROOT}/ngp/run_ngp.cpp"
    "${SRC_ROOT}/ngp/ngc_display.cpp"
    "${SRC_ROOT}/ngp/ngc_scheduler.cpp"
    "${SRC_ROOT}/ngp/race/graphics.c"
    "${SRC_ROOT}/ngp/race/race-memory.c"
)

set(COLECO_DEBUG_LOG_SRCS
    "${SRC_ROOT}/sms/run_sms.cpp"
    "${SRC_ROOT}/sms/smsplus/sms.c"
    "${SRC_ROOT}/gx4000/arnold/audioevent.c"
    "${SRC_ROOT}/gx4000/arnold/z80/z80.c"
)

set(A7800_LOG_SRCS
    "${SRC_ROOT}/atari7800/core/core/BupChip.c"
    "${SRC_ROOT}/atari7800/core/core/libretro.c"
    "${SRC_ROOT}/atari7800/core/core/Maria.c"
    "${SRC_ROOT}/atari7800/core/core/Memory.c"
    "${SRC_ROOT}/atari7800/core/core/Pokey.c"
    "${SRC_ROOT}/atari7800/core/core/ProSystem.c"
    "${SRC_ROOT}/atari7800/core/core/Tia.c"
)

set(SNES_LOG_SRCS
    "${SRC_ROOT}/snes/run_snes.cpp"
    "${SRC_ROOT}/snes/snes_display.cpp"
    "${SRC_ROOT}/snes/snes_input.cpp"
    "${SRC_ROOT}/snes/snes_save.cpp"
    "${SRC_ROOT}/snes/snes_stubs.cpp"
    "${SRC_ROOT}/snes/snes9x/src/gfx.c"
    "${SRC_ROOT}/snes/snes9x/src/memmap.c"
)

set(SNES_DEEP_BENCH_SRCS
    "${SRC_ROOT}/snes/run_snes.cpp"
    "${SRC_ROOT}/snes/snes9x/src/gfx.c"
)

file(GLOB_RECURSE GX4000_LTO_SRCS
    "${SRC_ROOT}/gx4000/*.c"
    "${SRC_ROOT}/gx4000/*.cpp"
)

file(GLOB_RECURSE SMS_LTO_SRCS
    "${SRC_ROOT}/sms/*.c"
    "${SRC_ROOT}/sms/*.cpp"
)

file(GLOB_RECURSE NES_LTO_SRCS
    "${SRC_ROOT}/nes/*.c"
    "${SRC_ROOT}/nes/*.cpp"
    "${LIB_ROOT}/arduino-nofrendo/src/*.c"
)

file(GLOB_RECURSE A2600_LTO_SRCS
    "${SRC_ROOT}/atari2600/*.c"
    "${SRC_ROOT}/atari2600/*.cpp"
)

file(GLOB_RECURSE A7800_LTO_SRCS
    "${SRC_ROOT}/atari7800/*.c"
    "${SRC_ROOT}/atari7800/*.cpp"
)

file(GLOB_RECURSE PCE_LTO_SRCS
    "${SRC_ROOT}/pce/*.c"
    "${SRC_ROOT}/pce/*.cpp"
)

file(GLOB_RECURSE GBC_LTO_SRCS
    "${SRC_ROOT}/gbc/*.c"
    "${SRC_ROOT}/gbc/*.cpp"
)

file(GLOB_RECURSE LYNX_LTO_SRCS
    "${SRC_ROOT}/lynx/*.c"
    "${SRC_ROOT}/lynx/*.cpp"
)

file(GLOB_RECURSE MSX_LTO_SRCS
    "${SRC_ROOT}/msx/*.c"
    "${SRC_ROOT}/msx/*.cpp"
)

file(GLOB_RECURSE WS_LTO_SRCS
    "${SRC_ROOT}/ws/*.c"
    "${SRC_ROOT}/ws/*.cpp"
)

file(GLOB_RECURSE MD_LTO_SRCS
    "${SRC_ROOT}/genesis/*.c"
    "${SRC_ROOT}/genesis/*.cpp"
)

file(GLOB_RECURSE NGP_LTO_SRCS
    "${SRC_ROOT}/ngp/*.c"
    "${SRC_ROOT}/ngp/*.cpp"
)

file(GLOB_RECURSE SNES_LTO_SRCS
    "${SRC_ROOT}/snes/*.c"
    "${SRC_ROOT}/snes/*.cpp"
)

set(SNES_HOT_O3_SRCS
    "${SRC_ROOT}/snes/snes9x/src/cpuops.c"
    "${SRC_ROOT}/snes/snes9x/src/cpuexec.c"
    "${SRC_ROOT}/snes/snes9x/src/getset.c"
    "${SRC_ROOT}/snes/snes9x/src/ppu.c"
    "${SRC_ROOT}/snes/snes9x/src/gfx.c"
    "${SRC_ROOT}/snes/snes9x/src/tile.c"
)

set(SNES_CPU_MEM_OFAST_SRCS
    "${SRC_ROOT}/snes/snes9x/src/cpuops.c"
    "${SRC_ROOT}/snes/snes9x/src/getset.c"
)

if(ENABLE_GX4000_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${GX4000_LTO_SRCS})
endif()

if(ENABLE_SMS_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${SMS_LTO_SRCS})
endif()

if(ENABLE_NES_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${NES_LTO_SRCS})
endif()

if(ENABLE_A2600_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${A2600_LTO_SRCS})
endif()

if(ENABLE_A7800_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${A7800_LTO_SRCS})
endif()

if(ENABLE_PCE_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${PCE_LTO_SRCS})
endif()

if(ENABLE_GBC_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${GBC_LTO_SRCS})
endif()

if(ENABLE_LYNX_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${LYNX_LTO_SRCS})
endif()

if(ENABLE_MSX_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${MSX_LTO_SRCS})
endif()

if(ENABLE_WS_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${WS_LTO_SRCS})
endif()

if(ENABLE_MD_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${MD_LTO_SRCS})
endif()

if(ENABLE_NGP_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${NGP_LTO_SRCS})
endif()

if(ENABLE_SNES_CORE AND ENABLE_SNES_LTO)
    add_source_compile_opts(OPTIONS -flto -ffat-lto-objects SOURCES ${SNES_LTO_SRCS})
endif()

if(ENABLE_SNES_CORE AND ENABLE_SNES_NO_UNROLL_LOOPS)
    add_source_compile_opts(OPTIONS -fno-unroll-loops SOURCES ${SNES_LTO_SRCS})
endif()

if(ENABLE_SNES_CORE AND ENABLE_SNES_FAST_MATH)
    add_source_compile_opts(OPTIONS -ffast-math SOURCES ${SNES_LTO_SRCS})
endif()

if(ENABLE_SNES_CORE AND ENABLE_SNES_HOT_O3)
    add_source_compile_opts(OPTIONS -O3 SOURCES ${SNES_HOT_O3_SRCS})
endif()

if(ENABLE_SNES_CORE AND ENABLE_SNES_CPU_MEM_OFAST)
    add_source_compile_opts(OPTIONS -Ofast SOURCES ${SNES_CPU_MEM_OFAST_SRCS})
endif()

if(ENABLE_GX4000_LTO OR ENABLE_SMS_LTO OR ENABLE_NES_LTO OR ENABLE_A2600_LTO OR ENABLE_A7800_LTO OR ENABLE_PCE_LTO OR ENABLE_GBC_LTO OR ENABLE_LYNX_LTO OR ENABLE_MSX_LTO OR ENABLE_WS_LTO OR ENABLE_MD_LTO OR ENABLE_NGP_LTO OR (ENABLE_SNES_CORE AND ENABLE_SNES_LTO))
    idf_build_set_property(LINK_OPTIONS "-flto" APPEND)
    idf_build_set_property(LINK_OPTIONS "-fuse-linker-plugin" APPEND)
    if(ENABLE_EXPLICIT_LTO_PLUGIN)
        execute_process(
            COMMAND ${CMAKE_C_COMPILER} -print-file-name=liblto_plugin.dll
            OUTPUT_VARIABLE LTO_PLUGIN_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(EXISTS "${LTO_PLUGIN_PATH}")
            idf_build_set_property(LINK_OPTIONS "-Wl,-plugin,${LTO_PLUGIN_PATH}" APPEND)
        endif()
    endif()
endif()

if(ENABLE_EMU_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED SOURCES ${GENERAL_LOG_SRCS})
endif()

if(ENABLE_BOOT_DIAG_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED BOOT_DIAG_LOGS SOURCES ${BOOT_LOG_SRCS})
endif()

if(ENABLE_HEAP_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED HEAP_LOGS SOURCES "${SRC_ROOT}/main.cpp")
endif()

if(ENABLE_AUDIO_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED AUDIO_LOGS SOURCES ${AUDIO_LOG_SRCS})
endif()

if(ENABLE_MD_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED MD_LOGS SOURCES
        ${MD_AUDIO_LOG_SRCS}
        ${MD_RENDER_LOG_SRCS}
    )
endif()

if(ENABLE_MD_AUDIO_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED MD_AUDIO_LOGS SOURCES ${MD_AUDIO_LOG_SRCS})
endif()

if(ENABLE_MD_RENDER_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED MD_RENDER_LOGS SOURCES ${MD_RENDER_LOG_SRCS})
endif()

if(ENABLE_MSX_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED MSX_LOGS SOURCES ${MSX_LOG_SRCS})
endif()

if(ENABLE_SNES_CORE AND ENABLE_SNES_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED SNES_LOGS SOURCES ${SNES_LOG_SRCS})
endif()

if(ENABLE_SNES_CORE AND ENABLE_SNES_DEEP_BENCH)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED SNES_DEEP_BENCH SOURCES ${SNES_DEEP_BENCH_SRCS})
endif()

if(ENABLE_NES_DIAG_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED NES_DIAG_LOGS SOURCES ${NES_LOG_SRCS})
endif()

if(ENABLE_NES_BENCHMARK_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED NES_BENCHMARK_LOGS SOURCES ${NES_LOG_SRCS})
endif()

if(ENABLE_WS_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED WS_LOGS_ENABLED SOURCES ${WS_LOG_SRCS})
endif()

if(ENABLE_WS_BENCHMARK_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED WS_BENCHMARK_LOGS SOURCES ${WS_BENCHMARK_LOG_SRCS})
endif()

if(ENABLE_WS_MODEL_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED WS_MODEL_LOGS SOURCES
        "${SRC_ROOT}/ws/run_ws.cpp"
        "${SRC_ROOT}/ws/oswan/WSFileio.c"
    )
endif()

if(ENABLE_WS_RENDER_PROFILE)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED WS_BENCHMARK_LOGS WS_RENDER_PROFILE SOURCES
        "${SRC_ROOT}/ws/run_ws.cpp"
        "${SRC_ROOT}/ws/oswan/WS.c"
        "${SRC_ROOT}/ws/oswan/WSRender.c"
    )
endif()

if(ENABLE_WS_CPU_PROFILE)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED WS_BENCHMARK_LOGS WS_CPU_PROFILE SOURCES
        "${SRC_ROOT}/ws/run_ws.cpp"
        "${SRC_ROOT}/ws/oswan/cpu/nec.c"
    )
endif()

if(ENABLE_WS_CPU_BRANCH_PROFILE)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED WS_BENCHMARK_LOGS WS_CPU_BRANCH_PROFILE SOURCES
        "${SRC_ROOT}/ws/run_ws.cpp"
        "${SRC_ROOT}/ws/oswan/cpu/nec.c"
    )
endif()

if(ENABLE_NGP_TRACE_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED NGP_TRACE_LOGS SOURCES ${NGP_TRACE_LOG_SRCS})
endif()

if(ENABLE_COLECO_DEBUG_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED COLECO_DEBUG_LOGS SOURCES ${COLECO_DEBUG_LOG_SRCS})
endif()

if(ENABLE_A7800_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED A7800_LOGS SOURCES ${A7800_LOG_SRCS})
endif()

if(ENABLE_BENCHMARK_LOGS)
    add_source_compile_defs(DEFINITIONS EMU_LOGS_ENABLED BENCHMARK_LOGS SOURCES
        ${NES_LOG_SRCS}
        ${WS_BENCHMARK_LOG_SRCS}
    )
endif()

if(ENABLE_EMU_ALLOW_RAW_PRINTF)
    add_source_compile_defs(DEFINITIONS EMU_ALLOW_RAW_PRINTF SOURCES ${APP_SRCS})
endif()

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
    -Wno-implicit-fallthrough
    -Wno-maybe-uninitialized
    -Wno-unused-but-set-variable
    -Wno-unused-variable
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
# NES: nofrendo bitmap allocation wrappers.
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=bmp_create" APPEND)
# NES: nofrendo bitmap release wrappers.
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=bmp_destroy" APPEND)
