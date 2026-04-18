#include "run_videopac.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/md5.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cardputer/CardputerInput.h"
#include "cardputer/CardputerView.h"
#include "cardputer/VerticalSelector.h"
#include "share/display_target.h"
#include "share/emu_controls.h"
#include "share/utils.h"
#include "videopac_config.h"
#include "videopac_display.h"
#include "videopac_input.h"
#include "videopac_trace.h"

extern "C" {
    bool o2em_init(const uint8_t* bios_data, size_t bios_size, const char* bios_name,
                   const uint8_t* rom_data, size_t rom_size);
    void o2em_shutdown(void);
    void o2em_run_frame(void);
    void o2em_get_video(uint16_t** out_buffer, int* out_width, int* out_height, int* out_pitch_pixels);
    void o2em_get_video_indexed(const uint8_t** out_buffer,
                                int* out_width,
                                int* out_height,
                                int* out_pitch_pixels,
                                uint16_t out_palette[256]);
    void o2em_set_plus_external_native(bool enabled);
    void o2em_set_joystick(bool up, bool down, bool left, bool right, bool action);
    void o2em_set_key(char key, bool pressed);
    void o2em_get_audio(int16_t** out_buffer, size_t* out_samples);
    void o2em_get_system_av_info(double* out_fps, double* out_sample_rate);
}

namespace {

constexpr const char* kVideopacBiosSdDir = "/bios/videopac";
constexpr const char* kVideopacBiosVfsDir = "/sd/bios/videopac";
constexpr size_t kVideopacBiosSize = 1024;
constexpr int kVideopacPendingBiosBase = 100;
constexpr double kExternalFitPalVideoFps = 25.0;
constexpr double kExternalFitNtscVideoFps = 24.0;
constexpr uint32_t kExternalFitMaxSkippedFrames = 2;
constexpr int64_t kInputModeOverlayHoldUs = 650000;

constexpr VideopacBiosChoice kVideopacBioses[] = {
    {VideopacBiosId::Odyssey2Ntsc,       "Odyssey2 NTSC",    "o2rom.bin",  "o2rom.bin",  "562d5ebf9e030a40d6fabfc2f33139fd"},
    {VideopacBiosId::VideopacPal,        "Videopac PAL",     "c52.bin",    "c52.bin",    "f1071cdb0b6b10dde94d3bc8a6146387"},
    {VideopacBiosId::VideopacPlusG7400,  "Videopac+ G7400",  "g7400.bin",  "g7400.bin",  "c500ff71236068e0dc0d0603d265ae76"},
    {VideopacBiosId::VideopacPlusFrance, "Videopac+ France", "jopac.bin",  "jopac.bin",  "279008e4a0db2dc5f1c048853b033828"},
};

constexpr size_t kVideopacBiosCount = sizeof(kVideopacBioses) / sizeof(kVideopacBioses[0]);

size_t bios_index(VideopacBiosId id)
{
    const size_t index = static_cast<size_t>(id);
    return index < kVideopacBiosCount ? index : 0;
}

bool rom_name_has_bin_ext(const char* romName)
{
    if (!romName) {
        return false;
    }

    std::string value(romName);
    const size_t dot = value.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }

    std::string ext = value.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == "bin";
}

std::string rom_title_without_extension(const char* romName)
{
    if (!romName || romName[0] == '\0') {
        return "VIDEOPAC";
    }

    std::string title(romName);
    const size_t slash = title.find_last_of("/\\");
    if (slash != std::string::npos) {
        title = title.substr(slash + 1);
    }

    const size_t dot = title.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = title.substr(dot + 1);
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext == "bin" || ext == "o2") {
            title.erase(dot);
        }
    }

    return title.empty() ? "VIDEOPAC" : title;
}

std::string truncate_middle(const std::string& value, size_t maxChars)
{
    if (value.size() <= maxChars || maxChars <= 3) {
        return value.substr(0, maxChars);
    }

    const size_t left = (maxChars - 3) / 2;
    const size_t right = maxChars - 3 - left;
    return value.substr(0, left) + "..." + value.substr(value.size() - right);
}

void draw_internal_rom_title(const char* romName)
{
    auto& screen = M5Cardputer.Display;
    std::string title = rom_title_without_extension(romName);

    screen.setTextFont(1);
    screen.setTextSize(1);
    screen.setTextDatum(top_center);
    screen.setTextColor(TFT_ORANGE, TFT_BLACK);

    const int maxWidth = screen.width() - 10;
    size_t maxChars = title.size();
    while (screen.textWidth(title.c_str()) > maxWidth && maxChars > 8) {
        --maxChars;
        title = truncate_middle(rom_title_without_extension(romName), maxChars);
    }

    screen.fillRect(0, 27, screen.width(), 8, TFT_BLACK);
    screen.drawString(title.c_str(), screen.width() / 2, 27);
    screen.setTextDatum(middle_center);
}

bool compute_md5_hex(const uint8_t* data, size_t size, char out[33])
{
    if (!data || !out) {
        return false;
    }

    unsigned char digest[16] = {};
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);

    const int ok =
        (mbedtls_md5_starts_ret(&ctx) == 0 &&
         mbedtls_md5_update_ret(&ctx, data, size) == 0 &&
         mbedtls_md5_finish_ret(&ctx, digest) == 0);
    mbedtls_md5_free(&ctx);

    if (!ok) {
        return false;
    }

    for (size_t i = 0; i < sizeof(digest); ++i) {
        std::snprintf(out + (i * 2), 3, "%02x", digest[i]);
    }
    out[32] = '\0';
    return true;
}

const char* videopac_sd_open_path(const char* path)
{
    if (!path) {
        return "";
    }

    if (std::strncmp(path, "/sd/", 4) == 0) {
        return path + 3;
    }
    if (std::strcmp(path, "/sd") == 0) {
        return "/";
    }
    return path;
}

void log_directory_probe(const char* path)
{
    const char* openPath = videopac_sd_open_path(path);
    File dir = SD.open(openPath);
    if (!dir) {
        videopac_trace_printf("bios", "dir_probe open_failed path=%s open=%s", path, openPath);
        return;
    }
    if (!dir.isDirectory()) {
        videopac_trace_printf("bios", "dir_probe not_directory path=%s open=%s", path, openPath);
        dir.close();
        return;
    }

    videopac_trace_printf("bios", "dir_probe begin path=%s open=%s", path, openPath);
    dir.rewindDirectory();
    for (int i = 0; i < 16; ++i) {
        bool isDir = false;
        String name = dir.getNextFileName(&isDir);
        if (!name.length()) {
            break;
        }
        videopac_trace_printf("bios", "dir_probe entry=%s type=%s",
                              name.c_str(),
                              isDir ? "dir" : "file");
    }
    dir.close();
}

bool is_open_file(File& file)
{
    if (!file) {
        return false;
    }
    if (file.isDirectory()) {
        file.close();
        return false;
    }
    return true;
}

void show_launch_error(const char* line1, const char* line2)
{
    videopac_trace_printf("launch", "error line1=%s line2=%s",
                          line1 ? line1 : "",
                          line2 ? line2 : "");
    CardputerView display;
    display.initialize();
    display.topBar("VIDEOPAC ERROR", false, false);
    display.subMessage(line1 ? line1 : "Launch failed", line2 ? line2 : "", 2500);
}

void request_quit_to_launcher()
{
    videopac_trace_mark("launch", "quit_to_launcher requested");
    Preferences prefs;
    prefs.begin("cardputer_emu", false);
    prefs.putBool("quit_game", true);
    prefs.end();
    delay(50);
    esp_restart();
}

} // namespace

const VideopacBiosChoice& videopac_bios_info(VideopacBiosId id)
{
    return kVideopacBioses[bios_index(id)];
}

std::string videopac_bios_sd_path(VideopacBiosId id)
{
    return std::string(kVideopacBiosSdDir) + "/" + videopac_bios_info(id).fileName;
}

std::string videopac_bios_vfs_path(VideopacBiosId id)
{
    return std::string(kVideopacBiosVfsDir) + "/" + videopac_bios_info(id).fileName;
}

bool videopac_select_bios_for_rom(CardputerView& display,
                                  CardputerInput& input,
                                  const std::string& romPath,
                                  VideopacBiosId* outId)
{
    if (!outId) {
        return false;
    }

    if (!rom_name_has_bin_ext(romPath.c_str())) {
        videopac_trace_printf("bios", "auto_select rom_ext=o2 bios=%s", kVideopacBioses[0].fileName);
        *outId = VideopacBiosId::Odyssey2Ntsc;
        return true;
    }

    VideopacTraceScope scope("bios", "select_menu");
    VerticalSelector selector(display, input);
    std::vector<std::string> options;
    std::vector<std::string> details;
    options.reserve(kVideopacBiosCount);
    details.reserve(kVideopacBiosCount);

    for (const auto& bios : kVideopacBioses) {
        options.emplace_back(bios.label);
        details.emplace_back(bios.detail);
    }

    display.initialize();
    input.flushInput(150);

    for (;;) {
        display.topBar("SELECT VIDEOPAC BIOS", false, false);
        const int selected = selector.select("Select BIOS", options,
                                             false, false, details,
                                             {}, false, true, false, 0, -2, -2);
        if (selected >= 0 && selected < static_cast<int>(options.size())) {
            videopac_trace_printf("bios", "selected index=%d label=%s file=%s",
                                  selected,
                                  kVideopacBioses[selected].label,
                                  kVideopacBioses[selected].fileName);
            *outId = kVideopacBioses[selected].id;
            return true;
        }
        if (selected == -2) {
            videopac_trace_mark("bios", "selection_aborted");
            return false;
        }
        display.subMessage("Choose a BIOS", "for this .BIN ROM", 900);
    }
}

bool videopac_load_bios_image(VideopacBiosId id,
                              VideopacBiosImage* outImage,
                              char* error,
                              size_t errorSize)
{
    if (!outImage) {
        return false;
    }

    VideopacTraceScope scope("bios", "load");
    videopac_free_bios_image(outImage);
    outImage->id = id;

    if (error && errorSize > 0) {
        error[0] = '\0';
    }

    const VideopacBiosChoice& bios = videopac_bios_info(id);
    const std::string sdPath = videopac_bios_sd_path(id);
    const std::string vfsPath = videopac_bios_vfs_path(id);
    std::string activeVfsPath = vfsPath;
    const char* openPath = videopac_sd_open_path(activeVfsPath.c_str());
    videopac_trace_printf("bios", "open sd_path=%s vfs_path=%s open=%s expected_md5=%s",
                          sdPath.c_str(),
                          activeVfsPath.c_str(),
                          openPath,
                          bios.expectedMd5);
    File biosFile = SD.open(openPath, FILE_READ);

    if (!is_open_file(biosFile) && rom_name_has_bin_ext(bios.fileName)) {
        std::string duplicateExtPath = vfsPath + ".bin";
        const char* duplicateOpenPath = videopac_sd_open_path(duplicateExtPath.c_str());
        videopac_trace_printf("bios", "exact_open_failed try_duplicate_ext vfs_path=%s open=%s",
                              duplicateExtPath.c_str(),
                              duplicateOpenPath);
        biosFile = SD.open(duplicateOpenPath, FILE_READ);
        if (is_open_file(biosFile)) {
            activeVfsPath = duplicateExtPath;
            openPath = videopac_sd_open_path(activeVfsPath.c_str());
            videopac_trace_printf("bios", "duplicate_ext_open_ok expected_name=%s used_open=%s",
                                  bios.fileName,
                                  openPath);
        }
    }

    if (!is_open_file(biosFile)) {
        videopac_trace_printf("bios", "open_failed vfs_path=%s open=%s", activeVfsPath.c_str(), openPath);
        log_directory_probe(kVideopacBiosVfsDir);
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "Missing %s", bios.fileName);
        }
        return false;
    }

    const size_t biosSize = static_cast<size_t>(biosFile.size());
    videopac_trace_printf("bios", "size file=%s size=%u", bios.fileName, static_cast<unsigned>(biosSize));
    if (biosSize != kVideopacBiosSize) {
        std::printf("[VIDEOPAC] BIOS %s has invalid size: %u (need %u)\n",
                    bios.fileName,
                    static_cast<unsigned>(biosSize),
                    static_cast<unsigned>(kVideopacBiosSize));
        biosFile.close();
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "Invalid size %s", bios.fileName);
        }
        return false;
    }

    uint8_t* biosData = static_cast<uint8_t*>(heap_caps_malloc(biosSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!biosData) {
        videopac_trace_mark("bios", "internal_alloc_failed fallback=generic_heap");
        biosData = static_cast<uint8_t*>(heap_caps_malloc(biosSize, MALLOC_CAP_8BIT));
    }
    if (!biosData) {
        videopac_trace_printf("bios", "alloc_failed size=%u", static_cast<unsigned>(biosSize));
        biosFile.close();
        return false;
    }

    const size_t read = biosFile.read(biosData, biosSize);
    biosFile.close();
    videopac_trace_printf("bios", "read bytes=%u", static_cast<unsigned>(read));
    if (read != biosSize) {
        free(biosData);
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "Read failed %s", bios.fileName);
        }
        return false;
    }

    char md5Hex[33] = {};
    if (!compute_md5_hex(biosData, biosSize, md5Hex)) {
        free(biosData);
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "MD5 failed %s", bios.fileName);
        }
        return false;
    }
    videopac_trace_printf("bios", "md5 file=%s got=%s", bios.fileName, md5Hex);

    if (std::strcmp(md5Hex, bios.expectedMd5) != 0) {
        std::printf("[VIDEOPAC] BIOS %s MD5 mismatch got=%s expected=%s\n",
                    bios.fileName,
                    md5Hex,
                    bios.expectedMd5);
        free(biosData);
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "MD5 mismatch %s", bios.fileName);
        }
        return false;
    }

    outImage->data = biosData;
    outImage->size = biosSize;
    std::snprintf(outImage->md5, sizeof(outImage->md5), "%s", md5Hex);
    std::printf("[VIDEOPAC] BIOS %s OK md5=%s\n", bios.fileName, md5Hex);
    return true;
}

void videopac_free_bios_image(VideopacBiosImage* image)
{
    if (!image) {
        return;
    }

    free(image->data);
    image->data = nullptr;
    image->size = 0;
    image->md5[0] = '\0';
}

int videopac_bios_to_pending_mode(VideopacBiosId id)
{
    return kVideopacPendingBiosBase + static_cast<int>(bios_index(id));
}

bool videopac_bios_from_pending_mode(int pendingMode, VideopacBiosId* outId)
{
    const int index = pendingMode - kVideopacPendingBiosBase;
    if (index < 0 || index >= static_cast<int>(kVideopacBiosCount)) {
        return false;
    }
    if (outId) {
        *outId = kVideopacBioses[index].id;
    }
    return true;
}

void run_videopac(const uint8_t* romData,
                  size_t romLen,
                  const char* romName,
                  SdService& sd,
                  const VideopacBiosImage& biosImage)
{
    (void)sd;
    videopac_trace_reset(romName ? romName : "(unnamed)");
    videopac_trace_printf("launch", "start rom=%s size=%u",
                          romName ? romName : "(unnamed)",
                          static_cast<unsigned>(romLen));

    const VideopacBiosChoice& bios = videopac_bios_info(biosImage.id);
    if (!biosImage.data || biosImage.size != kVideopacBiosSize) {
        show_launch_error("BIOS not preloaded", videopac_bios_vfs_path(biosImage.id).c_str());
        return;
    }
    videopac_trace_printf("bios", "preloaded label=%s file=%s size=%u md5=%s",
                          bios.label,
                          bios.fileName,
                          static_cast<unsigned>(biosImage.size),
                          biosImage.md5);

    const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
    const bool usePlusBios =
        biosImage.id == VideopacBiosId::VideopacPlusG7400 ||
        biosImage.id == VideopacBiosId::VideopacPlusFrance;
    const bool usePlusExternalNative = useExternal && usePlusBios;
    const VideopacVideoMode storedVideoMode = videopac_config_load_video_mode();
    VideopacVideoMode videoMode = storedVideoMode;
    if (usePlusExternalNative && storedVideoMode == VideopacVideoMode::Fit) {
        videoMode = VideopacVideoMode::FitFast;
        videopac_config_set_video_mode(videoMode, false);
        videopac_trace_printf("display",
                              "video_mode_auto mode=%s reason=plus_external_default",
                              videopac_config_video_mode_label(videoMode));
    }
    videopac_trace_printf("display", "target=%s video_mode=%s color_depth=%s",
                          useExternal ? "external" : "internal",
                          videopac_config_video_mode_label(videoMode),
                          g_emu_color_depth == EMU_COLOR_12BIT ? "12bit" : "16bit");

    const auto showControlScreen = [&]() {
        CardputerView display;
        display.initialize();
        display.topBar(useExternal ? "VIDEOPAC EXT TFT" : "VIDEOPAC INTERNAL", false, false);
        display.showControlBindings(
            share::emuControlActionLabels(share::EmuProfile::Videopac),
            share::emuControlKeyLabels(share::EmuProfile::Videopac),
            "\\ = JOY/KBD  GO = QUIT"
        );
        if (useExternal) {
            draw_internal_rom_title(romName);
        }

        if (!useExternal && !emu_is_aux_screen_locked()) {
            videopac_display_show_external_info(romName, bios.fileName);
        }
    };

    {
        VideopacTraceScope scope("ui", "control_screen");
        showControlScreen();
    }

    {
        VideopacTraceScope scope("display", "init");
        videopac_display_init(useExternal);
    }
    {
        VideopacTraceScope scope("input", "init");
        videopac_input_init();
    }

    bool coreReady = false;
    {
        VideopacTraceScope scope("core", "o2em_init");
        coreReady = o2em_init(biosImage.data, biosImage.size, bios.fileName, romData, romLen);
    }
    if (!coreReady) {
        o2em_shutdown();
        videopac_display_shutdown();
        show_launch_error("O2EM core init failed", "Check ROM and BIOS");
        return;
    }
    o2em_set_plus_external_native(usePlusExternalNative);

    double coreFps = 60.0;
    double coreSampleRate = 42240.0;
    {
        VideopacTraceScope scope("core", "av_info");
        o2em_get_system_av_info(&coreFps, &coreSampleRate);
    }
    if (coreFps < 1.0) {
        coreFps = 60.0;
    }
    if (coreSampleRate < 1.0) {
        coreSampleRate = 42240.0;
    }
    videopac_trace_printf("core", "av fps=%.2f sample_rate=%.2f",
                          coreFps,
                          coreSampleRate);

    {
        VideopacTraceScope scope("audio", "speaker_config");
        auto spkCfg = M5Cardputer.Speaker.config();
        spkCfg.sample_rate = static_cast<uint32_t>(coreSampleRate);
        spkCfg.stereo = true;
        spkCfg.dma_buf_len = 256;
        spkCfg.dma_buf_count = 6;
        M5Cardputer.Speaker.config(spkCfg);
        if (!M5Cardputer.Speaker.isRunning()) {
            M5Cardputer.Speaker.begin();
        }
    }

    bool quitRequested = false;
    const int64_t targetFrameTimeUs = static_cast<int64_t>(1000000.0 / coreFps);
    int64_t nextFrameTimeUs = esp_timer_get_time() + targetFrameTimeUs;
    uint32_t frameCount = 0;
    uint32_t lastLogMs = millis();
    int lastFrameW = 0;
    int lastFrameH = 0;
    int lastFramePitch = 0;
    size_t lastAudioFrames = 0;
    int64_t lastLatenessUs = 0;
    bool lastFrameSkipEnabled = false;
    VideopacVideoMode lastFrameSkipMode = VideopacVideoMode::Fast;
    double fitRenderAccumulator = 0.0;
    double lastFitTargetVideoFps = 0.0;
    uint32_t fitSkippedFrames = 0;
    int64_t inputModeOverlayHoldUntilUs = 0;
    bool inputModeOverlayVisible = false;

    videopac_trace_printf("loop", "start target_frame_us=%lld",
                          static_cast<long long>(targetFrameTimeUs));

    while (!quitRequested) {
        const uint64_t frameStartUs = videopac_trace_now_us();
        VideopacInputState inputState = {};

        uint64_t stageStartUs = videopac_trace_now_us();
        videopac_input_poll(&inputState);
        videopac_trace_frame_sample("input", videopac_trace_now_us() - stageStartUs);
        if (inputState.videoModeToggleRequested) {
            videopac_config_step_video_mode(inputState.videoModeDirection);
            videopac_trace_printf("display", "video_mode_toggle mode=%s",
                                  videopac_config_get_video_mode_label());
        }
        if (inputState.inputModeChanged) {
            videopac_display_show_input_mode_overlay(inputState.keyboardOnlyMode, useExternal);
            inputModeOverlayHoldUntilUs = esp_timer_get_time() + kInputModeOverlayHoldUs;
            inputModeOverlayVisible = true;
        }
        if (inputState.menuChanged) {
            if (inputState.menuVisible) {
                M5Cardputer.Speaker.stop();
                videopac_display_show_runtime_menu(useExternal);
            } else if (useExternal) {
                VideopacTraceScope scope("ui", "control_screen_restore");
                showControlScreen();
            }
        } else if (inputState.videoModeToggleRequested && inputState.menuVisible) {
            videopac_display_show_runtime_menu(useExternal);
        }
        if (inputState.menuVisible) {
            nextFrameTimeUs = esp_timer_get_time() + targetFrameTimeUs;
            delay(16);
            continue;
        }
        if (inputState.quitRequested) {
            videopac_trace_mark("input", "quit_requested");
            quitRequested = true;
            break;
        }

        stageStartUs = videopac_trace_now_us();
        o2em_set_joystick(inputState.up, inputState.down, inputState.left, inputState.right, inputState.action);
        for (int i = 0; i < 128; ++i) {
            o2em_set_key(static_cast<char>(i), inputState.keys[i]);
        }
        videopac_trace_frame_sample("keys", videopac_trace_now_us() - stageStartUs);

        stageStartUs = videopac_trace_now_us();
        o2em_run_frame();
        videopac_trace_frame_sample("core", videopac_trace_now_us() - stageStartUs);

        const VideopacVideoMode currentVideoMode = videopac_config_get_video_mode();
        const bool frameSkipEnabled =
            useExternal &&
            (currentVideoMode == VideopacVideoMode::Fit ||
             currentVideoMode == VideopacVideoMode::FitFast);
        if (frameSkipEnabled != lastFrameSkipEnabled ||
            (frameSkipEnabled && currentVideoMode != lastFrameSkipMode)) {
            lastFrameSkipEnabled = frameSkipEnabled;
            lastFrameSkipMode = currentVideoMode;
            fitRenderAccumulator = frameSkipEnabled ? coreFps : 0.0;
            lastFitTargetVideoFps = 0.0;
            fitSkippedFrames = 0;
            videopac_trace_printf("display", "frame_skip %s",
                                  frameSkipEnabled
                                      ? (currentVideoMode == VideopacVideoMode::FitFast
                                             ? "external_fit_fast_adaptive"
                                             : "external_fit_on")
                                      : "off");
        }
        bool renderThisFrame = true;
        if (frameSkipEnabled) {
            const double targetVideoFps =
                currentVideoMode == VideopacVideoMode::FitFast
                    ? coreFps
                    : (coreFps >= 55.0 ? kExternalFitNtscVideoFps : kExternalFitPalVideoFps);
            if (targetVideoFps != lastFitTargetVideoFps) {
                lastFitTargetVideoFps = targetVideoFps;
                videopac_trace_printf("display", "fit_target_video_fps=%.1f",
                                      targetVideoFps);
            }

            fitRenderAccumulator += targetVideoFps;
            const bool cadenceDue = fitRenderAccumulator >= coreFps;
            const bool forceRender = fitSkippedFrames >= kExternalFitMaxSkippedFrames;
            const int64_t preRenderLatenessUs = esp_timer_get_time() - nextFrameTimeUs;
            const bool alreadyLate = preRenderLatenessUs > (targetFrameTimeUs / 2);

            renderThisFrame = forceRender || (cadenceDue && !alreadyLate);
            if (renderThisFrame) {
                fitRenderAccumulator = cadenceDue
                    ? fitRenderAccumulator - coreFps
                    : 0.0;
                fitSkippedFrames = 0;
            } else {
                fitSkippedFrames++;
            }
        } else {
            fitSkippedFrames = 0;
        }

        bool forceOverlayClearRender = false;
        if (inputModeOverlayVisible && inputModeOverlayHoldUntilUs <= esp_timer_get_time()) {
            videopac_display_hide_input_mode_overlay(useExternal);
            inputModeOverlayVisible = false;
            inputModeOverlayHoldUntilUs = 0;
            forceOverlayClearRender = true;
            renderThisFrame = true;
            fitSkippedFrames = 0;
        }

        const bool holdOverlay = inputModeOverlayVisible &&
                                 inputModeOverlayHoldUntilUs > esp_timer_get_time();
        if (renderThisFrame && !holdOverlay) {
            uint16_t* frameBuffer = nullptr;
            const uint8_t* indexedFrameBuffer = nullptr;
            uint16_t indexedPalette[256] = {};
            int frameW = 0;
            int frameH = 0;
            int framePitch = 0;
            stageStartUs = videopac_trace_now_us();
#if defined(FRONTEND_SUPPORTS_INDEXED_VIDEO)
            o2em_get_video_indexed(&indexedFrameBuffer, &frameW, &frameH, &framePitch, indexedPalette);
#else
            o2em_get_video(&frameBuffer, &frameW, &frameH, &framePitch);
#endif
            videopac_trace_frame_sample("video_get", videopac_trace_now_us() - stageStartUs);
            lastFrameW = frameW;
            lastFrameH = frameH;
            lastFramePitch = framePitch;

            stageStartUs = videopac_trace_now_us();
#if defined(FRONTEND_SUPPORTS_INDEXED_VIDEO)
            if (usePlusExternalNative) {
                videopac_display_render_indexed_plus_external(indexedFrameBuffer,
                                                              frameW,
                                                              frameH,
                                                              framePitch,
                                                              indexedPalette,
                                                              useExternal);
            } else {
                videopac_display_render_indexed(indexedFrameBuffer,
                                                frameW,
                                                frameH,
                                                framePitch,
                                                indexedPalette,
                                                useExternal);
            }
#else
            videopac_display_render(frameBuffer, frameW, frameH, framePitch, useExternal);
#endif
            videopac_trace_frame_sample("render", videopac_trace_now_us() - stageStartUs);
            if (forceOverlayClearRender) {
                videopac_trace_frame_sample("overlay_clear", 0);
            }
        } else if (holdOverlay) {
            videopac_trace_frame_sample("overlay_hold", 0);
        } else {
            videopac_trace_frame_sample("render_skip", 0);
        }

        int16_t* audioBuffer = nullptr;
        size_t audioFrames = 0;
        stageStartUs = videopac_trace_now_us();
        o2em_get_audio(&audioBuffer, &audioFrames);
        if (audioBuffer && audioFrames > 0) {
            while (M5Cardputer.Speaker.isPlaying(0) >= 3) {
                taskYIELD();
            }
            M5Cardputer.Speaker.playRaw(audioBuffer,
                                        audioFrames * 2,
                                        static_cast<uint32_t>(coreSampleRate),
                                        true,
                                        1,
                                        0,
                                        false);
        }
        videopac_trace_frame_sample("audio", videopac_trace_now_us() - stageStartUs);
        lastAudioFrames = audioFrames;

        const int64_t latenessUs = esp_timer_get_time() - nextFrameTimeUs;
        lastLatenessUs = latenessUs;
        stageStartUs = videopac_trace_now_us();
        if (latenessUs < 0) {
            share::sleep_until_us(nextFrameTimeUs);
        } else if (latenessUs > targetFrameTimeUs) {
            nextFrameTimeUs = esp_timer_get_time();
            taskYIELD();
        } else {
            taskYIELD();
        }
        videopac_trace_frame_sample("sleep", videopac_trace_now_us() - stageStartUs);

        nextFrameTimeUs += targetFrameTimeUs;
        videopac_trace_frame_sample("frame", videopac_trace_now_us() - frameStartUs);

        ++frameCount;
        const uint32_t nowMs = millis();
        if ((nowMs - lastLogMs) >= 2000) {
            const float fps = (frameCount * 1000.0f) / static_cast<float>(nowMs - lastLogMs);
            videopac_trace_frame_report(frameCount,
                                        fps,
                                        lastFrameW,
                                        lastFrameH,
                                        lastFramePitch,
                                        lastAudioFrames,
                                        lastLatenessUs);
            frameCount = 0;
            lastLogMs = nowMs;
        }
    }

    videopac_trace_mark("loop", "stop");
    {
        VideopacTraceScope scope("audio", "speaker_stop");
        M5Cardputer.Speaker.stop();
    }
    {
        VideopacTraceScope scope("core", "o2em_shutdown");
        o2em_set_plus_external_native(false);
        o2em_shutdown();
    }
    {
        VideopacTraceScope scope("display", "shutdown");
        videopac_display_shutdown();
    }

    if (quitRequested) {
        request_quit_to_launcher();
    }
}
