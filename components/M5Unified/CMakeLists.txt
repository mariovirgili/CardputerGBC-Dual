get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)

set(M5UNIFIED_ROOT "${PROJECT_ROOT}/.pio/libdeps/cardputer_adv_idf/M5Unified")
if(NOT EXISTS "${M5UNIFIED_ROOT}/src")
    set(M5UNIFIED_ROOT "${PROJECT_ROOT}/.pio/libdeps/m5stack-stamps3/M5Unified")
endif()
if(NOT EXISTS "${M5UNIFIED_ROOT}/src")
    message(FATAL_ERROR "M5Unified sources not found. Run PlatformIO once to install lib_deps.")
endif()

if(DEFINED ENV{PIOENV})
    set(M5UNIFIED_GENERATED_ENV "$ENV{PIOENV}")
else()
    set(M5UNIFIED_GENERATED_ENV "cardputer_adv_idf")
endif()
set(M5UNIFIED_GENERATED_DIR "${PROJECT_ROOT}/.pio/build/${M5UNIFIED_GENERATED_ENV}/generated/M5Unified")
file(MAKE_DIRECTORY "${M5UNIFIED_GENERATED_DIR}")

function(cardputer_replace_between VAR START_MARKER END_MARKER REPLACEMENT)
    string(FIND "${${VAR}}" "${START_MARKER}" START_POS)
    string(FIND "${${VAR}}" "${END_MARKER}" END_POS)
    if(START_POS EQUAL -1 OR END_POS EQUAL -1)
        message(FATAL_ERROR "Could not patch M5Unified.cpp for Cardputer ADV-only build")
    endif()
    string(SUBSTRING "${${VAR}}" 0 ${START_POS} PATCH_PREFIX)
    string(SUBSTRING "${${VAR}}" ${END_POS} -1 PATCH_SUFFIX)
    set(${VAR} "${PATCH_PREFIX}${REPLACEMENT}${PATCH_SUFFIX}" PARENT_SCOPE)
endfunction()

file(GLOB M5UNIFIED_SRCS
    "${M5UNIFIED_ROOT}/src/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/imu/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/led/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/power/*.cpp"
    "${M5UNIFIED_ROOT}/src/utility/rtc/*.cpp"
)

set(M5UNIFIED_MAIN_SRC "${M5UNIFIED_ROOT}/src/M5Unified.cpp")
set(M5UNIFIED_MAIN_PATCHED "${M5UNIFIED_GENERATED_DIR}/m5unified_cardputer_adv.cpp")
file(READ "${M5UNIFIED_MAIN_SRC}" M5UNIFIED_MAIN_CONTENT)
string(REPLACE "\r\n" "\n" M5UNIFIED_MAIN_CONTENT "${M5UNIFIED_MAIN_CONTENT}")

# Cardputer ADV uses M5Unified for display, speaker, power and BtnA only.
# Strip generic LED/touch/RTC/IMU update paths so their backend families do
# not get pulled into this fixed-board firmware.
set(M5UNIFIED_SETUP_LED_BEGIN "  void M5Unified::_setup_led(board_t board)\n  {")
set(M5UNIFIED_SETUP_LED_END "\n\n  void M5Unified::_begin(const config_t& cfg)\n  {")
set(M5UNIFIED_SETUP_LED_CARDPUTER [=[
  void M5Unified::_setup_led(board_t board)
  {
    (void)board;
  }

]=])
cardputer_replace_between(
    M5UNIFIED_MAIN_CONTENT
    "${M5UNIFIED_SETUP_LED_BEGIN}"
    "${M5UNIFIED_SETUP_LED_END}"
    "${M5UNIFIED_SETUP_LED_CARDPUTER}"
)

set(M5UNIFIED_AUDIO_BEGIN "  void M5Unified::_begin_audio(config_t& cfg)\n  {")
set(M5UNIFIED_AUDIO_END "\n\n  bool M5Unified::_begin_rtc_imu(const config_t& cfg)\n  {")
set(M5UNIFIED_AUDIO_CARDPUTER [=[
  void M5Unified::_begin_audio(config_t& cfg)
  {
    if (!cfg.internal_spk)
    {
      return;
    }

    auto spk_cfg = Speaker.config();
    spk_cfg.pin_bck = GPIO_NUM_41;
    spk_cfg.pin_ws = GPIO_NUM_43;
    spk_cfg.pin_data_out = GPIO_NUM_42;
    spk_cfg.magnification = 16;
    spk_cfg.i2s_port = I2S_NUM_1;
    Speaker.setCallback(this, _speaker_enabled_cb_cardputer_adv);
    Speaker.config(spk_cfg);
  }

]=])
cardputer_replace_between(
    M5UNIFIED_MAIN_CONTENT
    "${M5UNIFIED_AUDIO_BEGIN}"
    "${M5UNIFIED_AUDIO_END}"
    "${M5UNIFIED_AUDIO_CARDPUTER}"
)

set(M5UNIFIED_RTC_IMU_BEGIN "  bool M5Unified::_begin_rtc_imu(const config_t& cfg)\n  {")
set(M5UNIFIED_RTC_IMU_END "\n\n  void M5Unified::update( void )\n  {")
set(M5UNIFIED_RTC_IMU_CARDPUTER [=[
  bool M5Unified::_begin_rtc_imu(const config_t& cfg)
  {
    (void)cfg;
    return false;
  }

]=])
cardputer_replace_between(
    M5UNIFIED_MAIN_CONTENT
    "${M5UNIFIED_RTC_IMU_BEGIN}"
    "${M5UNIFIED_RTC_IMU_END}"
    "${M5UNIFIED_RTC_IMU_CARDPUTER}"
)

set(M5UNIFIED_UPDATE_BEGIN "  void M5Unified::update( void )\n  {")
set(M5UNIFIED_UPDATE_END "\n\n  void M5Unified::setTouchButtonHeightByRatio(uint8_t ratio)\n  {")
set(M5UNIFIED_UPDATE_CARDPUTER [=[
  void M5Unified::update( void )
  {
    auto ms = m5gfx::millis();
    _updateMsec = ms;
    _buttons[0].setRawState(ms, (!m5gfx::gpio_in(GPIO_NUM_0)) & 1);
  }

]=])
cardputer_replace_between(
    M5UNIFIED_MAIN_CONTENT
    "${M5UNIFIED_UPDATE_BEGIN}"
    "${M5UNIFIED_UPDATE_END}"
    "${M5UNIFIED_UPDATE_CARDPUTER}"
)

string(REPLACE
    "    // Touch screen operation is always limited to the first display.\n    Touch.begin(_displays.front().touch() ? &_displays.front() : nullptr);\n"
    "    // Cardputer ADV has no touch panel; avoid linking M5Unified touch backend.\n"
    M5UNIFIED_MAIN_CONTENT
    "${M5UNIFIED_MAIN_CONTENT}"
)
file(WRITE "${M5UNIFIED_MAIN_PATCHED}" "${M5UNIFIED_MAIN_CONTENT}")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/M5Unified\\.cpp$")
list(APPEND M5UNIFIED_SRCS "${M5UNIFIED_MAIN_PATCHED}")

set(M5UNIFIED_IMU_SRC "${M5UNIFIED_ROOT}/src/utility/IMU_Class.cpp")
set(M5UNIFIED_IMU_PATCHED "${M5UNIFIED_GENERATED_DIR}/imu_class_no_bmi270.cpp")
file(READ "${M5UNIFIED_IMU_SRC}" M5UNIFIED_IMU_CONTENT)
string(REPLACE "\r\n" "\n" M5UNIFIED_IMU_CONTENT "${M5UNIFIED_IMU_CONTENT}")
string(REPLACE
    "#include \"../M5Unified.hpp\""
    "#include \"M5Unified.hpp\""
    M5UNIFIED_IMU_CONTENT
    "${M5UNIFIED_IMU_CONTENT}"
)

# Cardputer ADV does not use M5Unified's IMU path. Keep the public IMU facade
# available, but remove the BMI270 probe and its large embedded config blob
# from the linked firmware.
string(REPLACE
    "#include \"imu/BMI270_Class.hpp\"\n"
    ""
    M5UNIFIED_IMU_CONTENT
    "${M5UNIFIED_IMU_CONTENT}"
)
set(M5UNIFIED_BMI270_BEGIN "    if (_imu == imu_t::imu_none)\n    {\n      auto bmi2 = new BMI270_Class();")
set(M5UNIFIED_BMI270_END "    if (_imu == imu_t::imu_none)\n    {\n      auto sh200q = new SH200Q_Class();")
string(FIND "${M5UNIFIED_IMU_CONTENT}" "${M5UNIFIED_BMI270_BEGIN}" M5UNIFIED_BMI270_BEGIN_POS)
string(FIND "${M5UNIFIED_IMU_CONTENT}" "${M5UNIFIED_BMI270_END}" M5UNIFIED_BMI270_END_POS)
if(M5UNIFIED_BMI270_BEGIN_POS EQUAL -1 OR M5UNIFIED_BMI270_END_POS EQUAL -1)
    message(FATAL_ERROR "Could not patch M5Unified IMU_Class.cpp to remove BMI270")
endif()
string(SUBSTRING "${M5UNIFIED_IMU_CONTENT}" 0 ${M5UNIFIED_BMI270_BEGIN_POS} M5UNIFIED_IMU_PREFIX)
string(SUBSTRING "${M5UNIFIED_IMU_CONTENT}" ${M5UNIFIED_BMI270_END_POS} -1 M5UNIFIED_IMU_SUFFIX)
set(M5UNIFIED_IMU_CONTENT "${M5UNIFIED_IMU_PREFIX}${M5UNIFIED_IMU_SUFFIX}")
file(WRITE "${M5UNIFIED_IMU_PATCHED}" "${M5UNIFIED_IMU_CONTENT}")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/utility/IMU_Class\\.cpp$")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/utility/imu/BMI270_Class\\.cpp$")
list(APPEND M5UNIFIED_SRCS "${M5UNIFIED_IMU_PATCHED}")

list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/utility/LED_Class\\.cpp$")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/utility/Touch_Class\\.cpp$")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/utility/RTC_Class\\.cpp$")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/utility/IMU_Class\\.cpp$")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/utility/led/.*\\.cpp$")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/utility/rtc/.*\\.cpp$")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/src/utility/imu/.*\\.cpp$")
list(FILTER M5UNIFIED_SRCS EXCLUDE REGEX ".*/generated/M5Unified/imu_class_no_bmi270\\.cpp$")

idf_component_register(
    SRCS ${M5UNIFIED_SRCS}
    INCLUDE_DIRS
        "${M5UNIFIED_ROOT}/src"
        "${M5UNIFIED_ROOT}/src/utility"
    REQUIRES M5GFX esp_adc driver
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-maybe-uninitialized
)
