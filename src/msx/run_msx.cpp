#include "run_msx.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <Preferences.h>
#include <SD.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "cardputer/CardputerInput.h"
#include "cardputer/CardputerView.h"
#include "cardputer/SdService.h"
#include "cardputer/VerticalSelector.h"
#include "core/msx_core.h"
#include "core/msx_disk.h"
#include "esp_timer.h"
#include "last_game.h"
#include "msx_config.h"
#include "msx_display.h"
#include "msx_input.h"
#include "msx_media.h"
#include "msx_video.h"
#include <M5Cardputer.h>
#include <TFT_eSPI.h>
#include "msx_sound.h"
#include "share/display_target.h"
#include "share/emu_controls.h"
#include "share/game_save.h"
#include "share/utils.h"
#include "vfs/rom_flash_io.h"
#include "vfs/rom_xip.h"

#ifndef MSX_RUN_LOG_ENABLED
#define MSX_RUN_LOG_ENABLED 0
#endif

#ifndef MSX_BOOT_LOG_ENABLED
#define MSX_BOOT_LOG_ENABLED 1
#endif

#if MSX_RUN_LOG_ENABLED
#define MSX_RUN_LOG(...) std::printf(__VA_ARGS__)
#else
#define MSX_RUN_LOG(...) do { } while (0)
#endif

#if MSX_BOOT_LOG_ENABLED
#define MSX_BOOT_LOG(...) std::printf(__VA_ARGS__)
#else
#define MSX_BOOT_LOG(...) do { } while (0)
#endif

extern uint8_t msx_input_get_state_slot(void);
extern bool msx_input_get_save_requested(void);
extern bool msx_input_get_load_requested(void);
extern bool msx_input_get_change_cas_requested(void);
bool msx_core_save_state(MsxCoreState* state, const char* path);
bool msx_core_load_state(MsxCoreState* state, const char* path);

namespace {

struct MsxViewModeOverrideGuard {
    MsxViewModeOverrideGuard()
    {
        msx_config_clear_view_mode_override();
    }

    ~MsxViewModeOverrideGuard()
    {
        msx_config_clear_view_mode_override();
    }

    void configureForTarget(bool useExternal) const
    {
        if (useExternal) {
            msx_config_set_view_mode_override(MsxInternalViewMode::PixelPerfect);
            return;
        }

        msx_config_clear_view_mode_override();
    }
};

struct MsxBiosReferenceEntry {
    const char* name;
    const char* md5;
};

struct MsxRuntimeTimingWindow {
    uint32_t frames;
    uint64_t frameUs;
    uint64_t cpuUs;
    uint64_t vdpUs;
    uint64_t presentUs;
    uint64_t otherUs;
};

struct MsxFpsOverlayWindow {
    uint32_t frames;
    uint32_t windowStartMs;
};

uint32_t msx_runtime_avg_us(uint64_t totalUs, uint32_t frames)
{
    return frames != 0u ? static_cast<uint32_t>(totalUs / frames) : 0u;
}

void msx_runtime_reset_fps_overlay(MsxFpsOverlayWindow* window, uint32_t nowMs)
{
    if (!window) {
        return;
    }

    window->frames = 0u;
    window->windowStartMs = nowMs;
}

void msx_runtime_update_fps_overlay(MsxFpsOverlayWindow* window,
                                    bool menuPaused,
                                    uint32_t nowMs)
{
    if (!window) {
        return;
    }

    if (menuPaused) {
        msx_runtime_reset_fps_overlay(window, nowMs);
        return;
    }

    if (window->windowStartMs == 0u) {
        window->windowStartMs = nowMs;
    }

    window->frames++;
    const uint32_t elapsedMs = nowMs - window->windowStartMs;
    if (elapsedMs < 250u) {
        return;
    }

    const uint32_t fps10 =
        elapsedMs != 0u
            ? static_cast<uint32_t>(
                  std::min<uint64_t>(
                      9999u,
                      (static_cast<uint64_t>(window->frames) * 10000ull + (elapsedMs / 2u)) / elapsedMs))
            : 0u;
    msx_video_set_fps_overlay_value(static_cast<uint16_t>(fps10));
    window->frames = 0u;
    window->windowStartMs = nowMs;
}

void msx_runtime_timing_add(MsxRuntimeTimingWindow* window, const MsxCoreState* core)
{
    if (!window || !core) {
        return;
    }

    window->frames++;
    window->frameUs += core->lastFrameTotalUs;
    window->cpuUs += core->lastFrameCpuUs;
    window->vdpUs += core->lastFrameVdpUs;
    window->presentUs += core->lastFramePresentUs;
    window->otherUs += core->lastFrameOtherUs;
}

void msx_runtime_log_summary(const MsxCoreState* core,
                             const MsxAudioHookState* audioState,
                             uint32_t frameCount,
                             uint32_t elapsedMs,
                             MsxRuntimeTimingWindow* timingWindow,
                             const char* suffix = nullptr)
{
    if (!core || !audioState || elapsedMs == 0u) {
        return;
    }

    const float fps = (frameCount * 1000.0f) / static_cast<float>(elapsedMs);
    const uint32_t timedFrames = timingWindow ? timingWindow->frames : 0u;
    const uint32_t avgFrameUs =
        msx_runtime_avg_us(timingWindow ? timingWindow->frameUs : 0u, timedFrames);
    const uint32_t avgCpuUs =
        msx_runtime_avg_us(timingWindow ? timingWindow->cpuUs : 0u, timedFrames);
    const uint32_t avgVdpUs =
        msx_runtime_avg_us(timingWindow ? timingWindow->vdpUs : 0u, timedFrames);
    const uint32_t avgPresentUs =
        msx_runtime_avg_us(timingWindow ? timingWindow->presentUs : 0u, timedFrames);
    const uint32_t avgOtherUs =
        msx_runtime_avg_us(timingWindow ? timingWindow->otherUs : 0u, timedFrames);
    const char* viewLabel =
        msx_config_get_active_view_mode_label_for_target(g_emu_display_target == EMU_DISPLAY_EXTERNAL);

    if (suffix && suffix[0] != '\0') {
        MSX_RUN_LOG("[MSX] FPS %.1f | FT %u | CPU %u | RND %u | PRS %u | OTH %u | HEAP %u | CPU %s | PC %04X | VDP %s | MACHINE %s | VIEW %s | PERF %s | AUDIOQ %u | %s\n",
                    fps,
                    avgFrameUs,
                    avgCpuUs,
                    avgVdpUs,
                    avgPresentUs,
                    avgOtherUs,
                    esp_get_free_heap_size(),
                    msx_cpu_run_state_label(core->cpu.runState),
                    core->cpu.pc,
                    msx_vdp_mode_label(core->vdp.mode),
                    msx_media_bios_target_label(core->biosTarget),
                    viewLabel,
                    msx_config_get_performance_mode_label(),
                    static_cast<unsigned>(audioState->queuedBlocks),
                    suffix);
    } else {
        MSX_RUN_LOG("[MSX] FPS %.1f | FT %u | CPU %u | RND %u | PRS %u | OTH %u | HEAP %u | CPU %s | PC %04X | VDP %s | MACHINE %s | VIEW %s | PERF %s | AUDIOQ %u\n",
                    fps,
                    avgFrameUs,
                    avgCpuUs,
                    avgVdpUs,
                    avgPresentUs,
                    avgOtherUs,
                    esp_get_free_heap_size(),
                    msx_cpu_run_state_label(core->cpu.runState),
                    core->cpu.pc,
                    msx_vdp_mode_label(core->vdp.mode),
                    msx_media_bios_target_label(core->biosTarget),
                    viewLabel,
                    msx_config_get_performance_mode_label(),
                    static_cast<unsigned>(audioState->queuedBlocks));
    }

    if (timingWindow) {
        *timingWindow = {};
    }
}

constexpr uint32_t kMsxSkeletonSampleRate = 22050;
constexpr uint8_t kMsxSkeletonChannels = 1;
constexpr double kMsxSkeletonFps = 60.0;
constexpr const char* kMsxBiosDirPrimary = "/sd/bios/msx/";
constexpr const char* kMsxBiosSdDirPrimary = "bios/msx/";
constexpr const char* kMsxBiosSdDirFallback = "msx/";
constexpr const char* kMsxBiosDirFallback = "/sd/msx/";

static const char* kMsxBiosReferenceTable =
    "MSX.ROM      364a1a579fe5cb8dba54519bcfcdac0d\n"
    "MSX2.ROM     (no md5)\n"
    "MSX2EXT.ROM  (no md5)\n"
    "DISK.ROM     80dcd1ad1a4cf65d64b7ba10504e8190\n"
    "MSXDOS2.ROM  6418d091cd6907bbcf940324339e43bb\n"
    "FMPAC.ROM    6f69cc8b5ed761b03afd78000dfb0e19\n";

constexpr MsxBiosReferenceEntry kMsxBiosReferences[] = {
    {"MSX.ROM",      "364a1a579fe5cb8dba54519bcfcdac0d"},
    {"MSX2.ROM",     ""},
    {"MSX2EXT.ROM",  ""},
    {"DISK.ROM",     "80dcd1ad1a4cf65d64b7ba10504e8190"},
    {"MSXDOS2.ROM",  "6418d091cd6907bbcf940324339e43bb"},
    {"FMPAC.ROM",    "6f69cc8b5ed761b03afd78000dfb0e19"},
};

const char* msx_sd_open_path(const char* path)
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

const char* msx_file_label(const char* path)
{
    if (!path || path[0] == '\0') {
        return "-";
    }

    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* base = slash;
    if (!base || (backslash && backslash > base)) {
        base = backslash;
    }

    return base ? base + 1 : path;
}

void msx_format_bios_status_line(const MsxBiosBundle& bios, char* out, size_t outSize)
{
    if (!out || outSize == 0) {
        return;
    }

    if (bios.subRomRequired && bios.subRom.status == MsxImageLoadStatus::Loaded &&
        bios.subRom.path[0] != '\0') {
        std::snprintf(out,
                      outSize,
                      "BIOS: %s + %s",
                      msx_file_label(bios.mainRom.path),
                      msx_file_label(bios.subRom.path));
        return;
    }

    std::snprintf(out, outSize, "BIOS: %s", msx_file_label(bios.mainRom.path));
}

static bool msx_path_has_cas_ext(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return false;
    }

    std::string ext = path.substr(dot + 1);
    for (char& ch : ext) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ext == "cas";
}

static std::string msx_join_browser_path(const std::string& folder, const std::string& entry)
{
    if (folder.empty() || folder == "/") {
        return "/" + entry;
    }
    if (folder.back() == '/') {
        return folder + entry;
    }
    return folder + "/" + entry;
}

static std::string msx_sd_open_string(const std::string& path)
{
    return msx_sd_open_path(path.c_str());
}

static std::string msx_cas_display_path_from_browser_path(const std::string& browserPath)
{
    if (browserPath.rfind("/sd/", 0) == 0 || browserPath == "/sd") {
        return browserPath;
    }
    return "/sd" + normalizeRomFolderPath(browserPath);
}

static std::string msx_initial_cas_display_path(const char* casName)
{
    const std::string savedPath = getLastGamePathFromNvs();
    if (!savedPath.empty() && msx_path_has_cas_ext(savedPath)) {
        const char* savedLabel = msx_file_label(savedPath.c_str());
        if (!casName || std::strcmp(savedLabel, casName) == 0) {
            return savedPath;
        }
    }

    return casName ? std::string(casName) : std::string();
}

static void msx_trim_spaces_in_place(std::string& value);

static std::string msx_title_without_extension(const char* fileName, const char* fallback)
{
    std::string title = msx_file_label(fileName);
    if (title.empty() && fallback) {
        title = fallback;
    }

    const size_t dot = title.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        title.resize(dot);
    }

    msx_trim_spaces_in_place(title);
    return title.empty() && fallback ? std::string(fallback) : title;
}

static void msx_trim_spaces_in_place(std::string& value)
{
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
}

static std::string msx_fit_title_line_no_ellipsis(const std::string& text, int maxWidth)
{
    std::string fitted = text;
    auto& display = M5Cardputer.Display;
    while (!fitted.empty() && display.textWidth(fitted.c_str()) > maxWidth) {
        fitted.pop_back();
    }
    msx_trim_spaces_in_place(fitted);
    return fitted;
}

static std::pair<std::string, std::string> msx_split_title_two_lines_no_ellipsis(const std::string& title,
                                                                                 int maxWidth)
{
    auto& display = M5Cardputer.Display;
    if (display.textWidth(title.c_str()) <= maxWidth) {
        return {title, std::string()};
    }

    size_t split = std::string::npos;
    for (size_t i = 1; i < title.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(title[i]))) {
            continue;
        }
        const std::string candidate = title.substr(0, i);
        if (display.textWidth(candidate.c_str()) <= maxWidth) {
            split = i;
        } else {
            break;
        }
    }

    std::string line1;
    std::string line2;
    if (split != std::string::npos) {
        line1 = title.substr(0, split);
        line2 = title.substr(split + 1);
    } else {
        line1 = msx_fit_title_line_no_ellipsis(title, maxWidth);
        line2 = title.substr(line1.size());
    }

    msx_trim_spaces_in_place(line1);
    msx_trim_spaces_in_place(line2);
    line2 = msx_fit_title_line_no_ellipsis(line2, maxWidth);
    return {line1, line2};
}

static void msx_draw_internal_title_two_lines(const std::string& title)
{
    auto& display = M5Cardputer.Display;
    display.fillRect(0, 0, display.width(), TOP_BAR_HEIGHT, BACKGROUND_COLOR);
    display.setFont(&fonts::Font0);
    display.setTextSize(TEXT_SMALL);
    display.setTextColor(TEXT_COLOR, BACKGROUND_COLOR);
    display.setTextDatum(top_left);

    const int maxWidth = display.width() - 8;
    const auto lines = msx_split_title_two_lines_no_ellipsis(title, maxWidth);
    const int centerX = display.width() / 2;

    if (lines.second.empty()) {
        const int x = (display.width() - display.textWidth(lines.first.c_str())) / 2;
        display.drawString(lines.first.c_str(), x, 11);
        return;
    }

    const int x1 = (display.width() - display.textWidth(lines.first.c_str())) / 2;
    const int x2 = (display.width() - display.textWidth(lines.second.c_str())) / 2;
    display.drawString(lines.first.c_str(), x1, 3);
    display.drawString(lines.second.c_str(), x2, 16);
    display.drawFastHLine(centerX - 12, 28, 24, PRIMARY_COLOR);
}

static void msx_show_launch_controls_panel(bool useExternal,
                                           const char* internalTitle,
                                           const char* romName,
                                           const char* fallbackRomTitle)
{
    CardputerView display;
    display.initialize();
    if (!useExternal) {
        display.topBar(internalTitle ? internalTitle : "MSX", false, false);
    }
    display.showControlBindings(
        share::emuControlActionLabels(share::EmuProfile::MSX),
        share::emuControlKeyLabels(share::EmuProfile::MSX),
        "GO = QUIT  HOLD GO = MENU"
    );

    if (useExternal) {
        msx_draw_internal_title_two_lines(
            msx_title_without_extension(romName, fallbackRomTitle ? fallbackRomTitle : "MSX")
        );
    }
}

const MsxBiosImage* msx_find_problem_bios_image(const MsxBiosBundle* bios)
{
    if (!bios) {
        return nullptr;
    }

    if (bios->subRom.status == MsxImageLoadStatus::Incompatible) {
        return &bios->subRom;
    }
    if (bios->mainRom.status == MsxImageLoadStatus::Incompatible) {
        return &bios->mainRom;
    }
    if (bios->subRomRequired && bios->subRom.status != MsxImageLoadStatus::Loaded) {
        return &bios->subRom;
    }
    if (bios->mainRom.status != MsxImageLoadStatus::Loaded) {
        return &bios->mainRom;
    }

    return nullptr;
}

void msx_split_md5(const char* md5, char first[17], char second[17])
{
    if (!first || !second) {
        return;
    }

    first[0] = '\0';
    second[0] = '\0';

    if (!md5 || md5[0] == '\0') {
        return;
    }

    std::snprintf(first, 17, "%.16s", md5);
    if (std::strlen(md5) > 16) {
        std::snprintf(second, 17, "%.16s", md5 + 16);
    }
}

void msx_request_quit_to_launcher(void)
{
    Preferences prefs;
    prefs.begin("cardputer_emu", false);
    prefs.putBool("quit_game", true);
    prefs.end();

    while (share::gameIsSaving()) {
        delay(1);
    }

    esp_restart();
}

void msx_show_launch_error(const char* title, const char* line1, const char* line2)
{
    CardputerView display;
    display.initialize();
    display.topBar(title ? title : "MSX ERROR", false, false);
    if (line1 && line1[0] != '\0') {
        display.subMessage(line1, 1200);
    }
    if (line2 && line2[0] != '\0') {
        display.subMessage(line2, 1400);
    }
}

void msx_wait_bios_page_ack(CardputerInput& input)
{
    input.flushInput(150);
    input.waitPress();
    input.flushInput(150);
}

void msx_draw_bios_page(CardputerView& display,
                        const char* title,
                        const char* const* lines,
                        size_t lineCount,
                        const char* footer)
{
    display.initialize();
    display.topBar(title ? title : "MSX BIOS", false, false);

    auto& screen = M5Cardputer.Display;
    const int boxX = 6;
    const int boxY = 36;
    const int boxW = screen.width() - 12;
    const int boxH = 92;
    const int textX = boxX + 8;
    const int firstLineY = boxY + 7;
    const int lineHeight = 10;

    screen.fillRoundRect(boxX, boxY, boxW, boxH, DEFAULT_ROUND_RECT, TFT_BLACK);
    screen.drawRoundRect(boxX, boxY, boxW, boxH, DEFAULT_ROUND_RECT, PRIMARY_COLOR);
    screen.setFont(&fonts::Font0);
    screen.setTextDatum(top_left);
    screen.setTextSize(1);

    int y = firstLineY;
    for (size_t i = 0; i < lineCount; ++i) {
        if (!lines[i] || lines[i][0] == '\0') {
            continue;
        }

        const bool highlight =
            (lines[i][0] == '/') ||
            (std::strncmp(lines[i], "MD5", 3) == 0) ||
            (std::strncmp(lines[i], "Need", 4) == 0) ||
            (std::strncmp(lines[i], "NEED", 4) == 0) ||
            (std::strncmp(lines[i], "AUTO", 4) == 0) ||
            (std::strncmp(lines[i], "Got", 3) == 0) ||
            (std::strncmp(lines[i], "FILE", 4) == 0);
        screen.setTextColor(highlight ? PRIMARY_COLOR : TEXT_COLOR, TFT_BLACK);
        screen.drawString(lines[i], textX, y);
        y += lineHeight;
    }

    if (footer && footer[0] != '\0') {
        screen.setTextDatum(middle_center);
        screen.setTextColor(PRIMARY_COLOR, TFT_BLACK);
        screen.drawCenterString(footer, screen.width() / 2, boxY + boxH - 12);
    }

    screen.setTextDatum(middle_center);
}

void msx_show_bios_reference_help(MsxMachineMode configuredMode, const MsxBiosBundle* bios)
{
    CardputerView display;
    CardputerInput input;

    std::printf("[MSX] BIOS reference files:\n%s", kMsxBiosReferenceTable);
    std::printf("[MSX] Expected BIOS directories on SD: %s or %s\n", kMsxBiosSdDirPrimary, kMsxBiosSdDirFallback);

    const MsxBiosImage* problemImage = msx_find_problem_bios_image(bios);
    if (problemImage &&
        problemImage->status == MsxImageLoadStatus::Incompatible &&
        problemImage->foundMd5[0] != '\0' &&
        problemImage->expectedMd5[0] != '\0') {
        char fileLine[32];
        char needLine[32];
        char gotMd5A[17];
        char gotMd5B[17];
        char needMd5A[17];
        char needMd5B[17];

        std::snprintf(fileLine, sizeof(fileLine), "FILE %s", msx_file_label(problemImage->path));
        std::snprintf(needLine,
                      sizeof(needLine),
                      "NEED %s",
                      problemImage->expectedName[0] != '\0'
                          ? problemImage->expectedName
                          : msx_file_label(problemImage->path));
        msx_split_md5(problemImage->foundMd5, gotMd5A, gotMd5B);
        msx_split_md5(problemImage->expectedMd5, needMd5A, needMd5B);

        const char* mismatchLines[8] = {
            fileLine,
            "Got MD5:",
            gotMd5A,
            gotMd5B,
            needLine,
            needMd5A,
            needMd5B,
            nullptr,
        };

        msx_draw_bios_page(display, "MSX BIOS MD5", mismatchLines, 7, "Press any key");
        msx_wait_bios_page_ack(input);
    }

    char needBiosLine[64];
    if (configuredMode == MsxMachineMode::MSX2) {
        std::snprintf(needBiosLine,
                      sizeof(needBiosLine),
                      "Need now: MSX2.ROM + MSX2EXT.ROM");
    } else if (configuredMode == MsxMachineMode::MSX1) {
        std::snprintf(needBiosLine, sizeof(needBiosLine), "Need now: MSX.ROM");
    } else {
        std::snprintf(needBiosLine,
                      sizeof(needBiosLine),
                      "Need now: MSX2.ROM + MSX2EXT.ROM or MSX.ROM");
    }

    const char* introLines[7] = {
        bios && bios->message[0] != '\0' ? bios->message : "Missing BIOS files.",
        "Place BIOS on SD in:",
        kMsxBiosSdDirPrimary,
        kMsxBiosSdDirFallback,
        needBiosLine,
        nullptr,
    };

    msx_draw_bios_page(display, "MSX BIOS ERROR", introLines, 7, "Press any key");
    msx_wait_bios_page_ack(input);

    for (size_t i = 0; i < (sizeof(kMsxBiosReferences) / sizeof(kMsxBiosReferences[0])); i += 2u) {
        char name1[24];
        char md51a[24];
        char md51b[24];
        char name2[24];
        char md52a[24];
        char md52b[24];
        char md5a[17];
        char md5b[17];

        std::snprintf(name1, sizeof(name1), "%s", kMsxBiosReferences[i].name);
        msx_split_md5(kMsxBiosReferences[i].md5, md5a, md5b);
        std::snprintf(md51a, sizeof(md51a), "MD5 %s", md5a);
        std::snprintf(md51b, sizeof(md51b), "    %s", md5b);

        const char* lines[6] = {name1, md51a, md51b, nullptr, nullptr, nullptr};
        size_t lineCount = 3;

        if (i + 1u < (sizeof(kMsxBiosReferences) / sizeof(kMsxBiosReferences[0]))) {
            std::snprintf(name2, sizeof(name2), "%s", kMsxBiosReferences[i + 1u].name);
            msx_split_md5(kMsxBiosReferences[i + 1u].md5, md5a, md5b);
            std::snprintf(md52a, sizeof(md52a), "MD5 %s", md5a);
            std::snprintf(md52b, sizeof(md52b), "    %s", md5b);
            lines[3] = name2;
            lines[4] = md52a;
            lines[5] = md52b;
            lineCount = 6;
        }

        msx_draw_bios_page(display,
                           "MSX BIOS LIST",
                           lines,
                           lineCount,
                           "Press any key");
        msx_wait_bios_page_ack(input);
    }
}

// Try to load a BIOS file (e.g. DISK.ROM) from the configured BIOS directories.
// Allocates with MALLOC_CAP_INTERNAL. Caller must free with heap_caps_free().
// Returns nullptr if the file is not found or has an unexpected size.
uint8_t* msx_load_bios_file(const char* filename, size_t expectedSize, size_t* outSize)
{
    if (!filename || expectedSize == 0) {
        return nullptr;
    }

    char pathBuf[128];
    const char* dirs[] = {kMsxBiosDirPrimary, kMsxBiosDirFallback};

    for (const char* dir : dirs) {
        std::snprintf(pathBuf, sizeof(pathBuf), "%s%s", dir, filename);
        const char* openPath = msx_sd_open_path(pathBuf);
        std::printf("[MSX] probing %s (open %s)\n", pathBuf, openPath);

        File f = SD.open(openPath, FILE_READ);
        if (!f) {
            std::printf("[MSX] %s: open failed\n", openPath);
            continue;
        }

        const size_t fileSize = static_cast<size_t>(f.size());
        std::printf("[MSX] %s: opened, size=%u expected=%u\n",
                    openPath,
                    static_cast<unsigned>(fileSize),
                    static_cast<unsigned>(expectedSize));
        if (fileSize != expectedSize) {
            std::printf("[MSX] %s: unexpected size %u (need %u)\n",
                        pathBuf, static_cast<unsigned>(fileSize), static_cast<unsigned>(expectedSize));
            f.close();
            continue;
        }

        uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(fileSize, MALLOC_CAP_8BIT));
        if (!buf) {
            buf = static_cast<uint8_t*>(heap_caps_malloc(fileSize, MALLOC_CAP_INTERNAL));
        }
        if (!buf) {
            buf = static_cast<uint8_t*>(heap_caps_malloc(fileSize, MALLOC_CAP_DEFAULT));
        }
        if (!buf) {
            std::printf("[MSX] %s: heap alloc failed (%u B) free8=%u largest8=%u freeInternal=%u largestInternal=%u\n",
                        openPath,
                        static_cast<unsigned>(fileSize),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
            f.close();
            continue;
        }

        const size_t read = f.read(buf, fileSize);
        f.close();

        if (read != fileSize) {
            std::printf("[MSX] %s: read %u of %u bytes\n",
                        pathBuf, static_cast<unsigned>(read), static_cast<unsigned>(fileSize));
            heap_caps_free(buf);
            continue;
        }

        std::printf("[MSX] loaded %s (%u B) from %s\n",
                    filename, static_cast<unsigned>(fileSize), pathBuf);
        if (outSize) {
            *outSize = fileSize;
        }
        return buf;
    }

    std::printf("[MSX] %s not found in bios directories\n", filename);
    return nullptr;
}

static String msx_get_savestate_path(const char* romName, uint8_t slot) {
    String name(romName);
    int dot = name.lastIndexOf('/');
    if (dot >= 0) name = name.substring(dot + 1);
    dot = name.lastIndexOf('\\');
    if (dot >= 0) name = name.substring(dot + 1);
    dot = name.lastIndexOf('.');
    if (dot > 0) name = name.substring(0, dot);
    
    String cleanName = "";
    for (int i = 0; i < name.length(); i++) {
        char c = name[i];
        if (c == '(' || c == '[' || c == '{') break;
        if (isalnum(c)) cleanName += c;
    }
    if (cleanName.length() == 0) cleanName = "default";
    if (cleanName.length() > 8) cleanName = cleanName.substring(0, 8);

    if (!SD.exists("/msx")) {
        bool ok = SD.mkdir("/msx");
        std::printf("[MSX][STATE] mkdir /msx %s\n", ok ? "OK" : "FAIL");
    }
    if (!SD.exists("/msx/states")) {
        bool ok = SD.mkdir("/msx/states");
        std::printf("[MSX][STATE] mkdir /msx/states %s\n", ok ? "OK" : "FAIL");
    }
    
    String path = "/msx/states/" + cleanName;
    if (!SD.exists(path)) {
        bool ok = SD.mkdir(path);
        std::printf("[MSX][STATE] mkdir %s %s\n", path.c_str(), ok ? "OK" : "FAIL");
    }

    return path + "/Slot" + String(slot) + ".sav";
}

static void msx_draw_osd_message(const char* msg, bool useExternal) {
    if (useExternal) {
        msx_video_lock();
        msx_video_prepare_external_ui();
        auto& tft = msx_video_external_tft();
        tft.fillRoundRect(80, 105, 160, 30, 4, TFT_BLACK);
        tft.drawRoundRect(80, 105, 160, 30, 4, PRIMARY_COLOR);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawCentreString(msg, 160, 113, 2);
        msx_video_unlock();
    } else {
        M5Cardputer.Display.fillRoundRect(60, 57, 120, 20, 4, TFT_BLACK);
        M5Cardputer.Display.drawRoundRect(60, 57, 120, 20, 4, PRIMARY_COLOR);
        M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5Cardputer.Display.drawCenterString(msg, 120, 62, &fonts::Font0);
    }
}

static void msx_begin_state_overlay(bool useExternal)
{
    (void)useExternal;
    msx_video_lock();
    msx_video_set_state_overlay_active(true);
    msx_video_unlock();
}

static void msx_end_state_overlay(bool useExternal)
{
    msx_video_lock();
    if (useExternal) {
        msx_video_finish_external_ui();
    }
    msx_video_set_state_overlay_active(false);
    msx_video_unlock();

    msx_video_request_full_redraw();
}

static void msx_apply_runtime_view_toggle(MsxCoreState* core, bool useExternal)
{
    msx_config_toggle_active_view_mode_for_target(useExternal);
    msx_video_request_full_redraw();
    if (core) {
        core->vdp.dirty = true;
        if (core->displayFrame.indexed8) {
            msx_video_present_frame(&core->displayFrame);
        }
    }
}

static bool msx_sd_root_accessible(void)
{
    File root = SD.open("/");
    const bool ok = root && root.isDirectory();
    if (root) {
        root.close();
    }
    return ok;
}

static bool msx_prepare_sd_for_state(SdService& sd)
{
    msx_video_prepare_sd_access();
    delay(5);

    const bool mounted = sd.getSdState();
    std::printf("[MSX][STATE] SD prep heap=%u largest=%u mounted=%s\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                mounted ? "yes" : "no");

    if (msx_sd_root_accessible()) {
        std::printf("[MSX][STATE] SD ready (existing mount) heap=%u largest=%u\n",
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        return true;
    }

    if (mounted) {
        std::printf("[MSX][STATE] SD root probe failed; keeping existing mount state\n");
        return false;
    }

    const bool ok = sd.begin();
    std::printf("[MSX][STATE] SD %s heap=%u largest=%u\n",
                ok ? "ready" : "remount failed",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return ok;
}

static void msx_filter_cas_browser_elements(SdService& sd,
                                            const std::string& folder,
                                            std::vector<std::string>& elements)
{
    std::vector<std::string> filtered;
    filtered.reserve(elements.size());
    for (const std::string& name : elements) {
        const std::string path = msx_join_browser_path(folder, name);
        if (sd.isDirectory(path) || msx_path_has_cas_ext(name)) {
            filtered.push_back(name);
        }
    }
    elements.swap(filtered);
}

static bool msx_select_cas_file(SdService& sd,
                                const std::string& currentCasPath,
                                std::string* selectedDisplayPath,
                                size_t* selectedSize)
{
    if (!selectedDisplayPath || !selectedSize) {
        return false;
    }

    CardputerView display;
    CardputerInput input;
    VerticalSelector selector(display, input);
    const std::vector<std::string> casExts = {".cas"};
    const std::string normalizedCurrentPath = normalizeRomBrowserPath(currentCasPath);
    const std::string originalFolder = extractRomFolder(normalizedCurrentPath);
    const std::string originalName = msx_file_label(normalizedCurrentPath.c_str());
    std::string currentFolder = originalFolder.empty() ? "/" : originalFolder;
    std::string previousFolder;
    std::vector<std::string> elements;

    display.initialize();
    input.flushInput(80);

    while (true) {
        if (currentFolder != previousFolder) {
            display.topBar("CHANGE CAS", true, true);
            display.subMessage("Loading tapes...", 0);
            elements = sd.getCachedDirectoryElements(currentFolder, &casExts, 1024);
            msx_filter_cas_browser_elements(sd, currentFolder, elements);
            if (currentFolder != "/") {
                elements.insert(elements.begin(), "..");
            }
            previousFolder = currentFolder;
        }

        if (elements.empty()) {
            display.topBar("CHANGE CAS", true, false);
            display.subMessage("No CAS files found", 1000);
            input.flushInput(120);
            return false;
        }

        int initialIndex = 0;
        if (currentFolder == originalFolder) {
            for (size_t i = 0; i < elements.size(); ++i) {
                if (elements[i] == originalName) {
                    initialIndex = static_cast<int>(i);
                    break;
                }
            }
        }

        const int selected = selector.select(
            currentFolder,
            elements,
            true,
            true,
            {},
            {},
            false,
            false,
            true,
            initialIndex,
            -1,
            -1
        );

        if (selected < 0 || selected >= static_cast<int>(elements.size())) {
            input.flushInput(120);
            return false;
        }

        const std::string& entry = elements[static_cast<size_t>(selected)];
        if (entry == "..") {
            const std::string parent = sd.getParentDirectory(currentFolder);
            currentFolder = parent.empty() ? "/" : parent;
            previousFolder.clear();
            continue;
        }

        const std::string browserPath = msx_join_browser_path(currentFolder, entry);
        if (sd.isDirectory(browserPath)) {
            currentFolder = browserPath;
            previousFolder.clear();
            continue;
        }

        if (!msx_path_has_cas_ext(browserPath)) {
            display.subMessage("Select a .CAS file", 700);
            input.flushInput(120);
            continue;
        }

        display.topBar("CHANGE CAS", true, false);
        display.subMessage("Preparing tape...", 0);
        size_t casSize = 0;
        if (!sd.getFileSize(browserPath, casSize) || casSize < 8u) {
            display.subMessage("Invalid CAS file", 900);
            input.flushInput(120);
            continue;
        }

        *selectedDisplayPath = msx_cas_display_path_from_browser_path(browserPath);
        *selectedSize = casSize;
        input.flushInput(120);
        return true;
    }
}

static bool msx_handle_change_cas(MsxCoreState* core,
                                  std::string& currentCasPath,
                                  std::vector<uint8_t>& runtimeCasBuffer,
                                  bool useExternal,
                                  SdService& sd)
{
    msx_begin_state_overlay(useExternal);
    msx_draw_osd_message("CHANGE CAS...", useExternal);

    bool changed = false;
    bool xipTouched = false;
    std::string selectedPath;
    size_t selectedSize = 0;

    if (msx_prepare_sd_for_state(sd) &&
        msx_select_cas_file(sd, currentCasPath, &selectedPath, &selectedSize)) {
        const esp_partition_t* romPart = findRomPartition("spiffs");
        if (!romPart) {
            std::printf("[MSX][CAS] change failed: ROM partition not found\n");
        } else if (selectedSize > romPart->size) {
            std::printf("[MSX][CAS] change failed: file too large size=%u partition=%u\n",
                        static_cast<unsigned>(selectedSize),
                        static_cast<unsigned>(romPart->size));
        } else {
            msx_draw_osd_message("FLASHING CAS...", useExternal);
            xipTouched = true;
            xip_unmap();
            size_t copiedSize = 0;
            if (!copyFileToPartition(selectedPath.c_str(), romPart, &copiedSize, nullptr, nullptr)) {
                std::printf("[MSX][CAS] change failed: copy to XIP partition failed path=%s\n",
                            selectedPath.c_str());
            } else if (xip_map_rom_partition("spiffs", copiedSize) != 0 || !get_rom_ptr()) {
                std::printf("[MSX][CAS] change failed: XIP remap failed size=%u\n",
                            static_cast<unsigned>(copiedSize));
            } else {
                runtimeCasBuffer.clear();
                runtimeCasBuffer.shrink_to_fit();
                const char* selectedName = msx_file_label(selectedPath.c_str());
                changed = msx_core_change_cas(core,
                                              get_rom_ptr(),
                                              get_rom_size(),
                                              selectedName);
                if (changed) {
                    currentCasPath = selectedPath;
                }
            }
        }
    }

    if (!changed && xipTouched) {
        (void)msx_core_change_cas(core, nullptr, 0, "MSX CAS");
    }

    msx_draw_osd_message(changed ? "CAS CHANGED" : "CAS UNCHANGED", useExternal);
    delay(500);
    msx_end_state_overlay(useExternal);
    return changed;
}

static void msx_handle_save_state(MsxCoreState* core, const char* stateName, bool useExternal, SdService& sd)
{
    msx_begin_state_overlay(useExternal);
    msx_draw_osd_message("SAVING STATE...", useExternal);
    bool saved = false;
    bool savedToRoot = false;

    if (msx_prepare_sd_for_state(sd)) {
        const uint8_t stateSlot = msx_input_get_state_slot();
        String path = msx_get_savestate_path(stateName, stateSlot);
        if (msx_core_save_state(core, path.c_str())) {
            saved = true;
        } else {
            String fallbackPath = "/msx_slot" + String(stateSlot) + ".sav";
            std::printf("[MSX][STATE] Fallback path: %s\n", fallbackPath.c_str());
            if (msx_core_save_state(core, fallbackPath.c_str())) {
                saved = true;
                savedToRoot = true;
            }
        }
    }

    if (saved) {
        M5Cardputer.Speaker.tone(3000, 100);
        msx_draw_osd_message(savedToRoot ? "SAVED TO ROOT" : "STATE SAVED", useExternal);
    } else {
        msx_draw_osd_message("SAVE FAILED", useExternal);
    }

    delay(500);
    msx_end_state_overlay(useExternal);
}

static void msx_handle_load_state(MsxCoreState* core, const char* stateName, bool useExternal, SdService& sd)
{
    msx_begin_state_overlay(useExternal);
    msx_draw_osd_message("LOADING STATE...", useExternal);
    bool loaded = false;
    bool loadedFromRoot = false;

    if (msx_prepare_sd_for_state(sd)) {
        const uint8_t stateSlot = msx_input_get_state_slot();
        String path = msx_get_savestate_path(stateName, stateSlot);
        if (msx_core_load_state(core, path.c_str())) {
            loaded = true;
        } else {
            String fallbackPath = "/msx_slot" + String(stateSlot) + ".sav";
            if (msx_core_load_state(core, fallbackPath.c_str())) {
                loaded = true;
                loadedFromRoot = true;
            }
        }
    }

    if (loaded) {
        M5Cardputer.Speaker.tone(3000, 100);
        msx_draw_osd_message(loadedFromRoot ? "LOADED FROM ROOT" : "STATE LOADED", useExternal);
    } else {
        msx_draw_osd_message("LOAD FAILED", useExternal);
    }

    delay(500);
    msx_end_state_overlay(useExternal);
}

} // namespace

void run_msx(const uint8_t* romData, size_t romLen, const char* romName, SdService& sd)
{
    const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
    MsxViewModeOverrideGuard viewModeGuard;
    msx_show_launch_controls_panel(
        useExternal,
        useExternal ? "MSX ON EXTERNAL TFT" : "MSX ON INTERNAL LCD",
        romName,
        "MSX ROM"
    );

    msx_config_load_internal_view_mode();
    msx_config_load_performance_mode();
    msx_config_load_fps_overlay_enabled();
    msx_config_load_frameskip_mode();
    viewModeGuard.configureForTarget(useExternal);
    const MsxMachineMode configuredMode = msx_config_load_machine_mode();
    msx_config_load_bios_path();
    msx_config_load_msx1_bios_path();
    msx_config_load_msx2_bios_path();
    msx_config_load_msx2_subrom_path();

    MsxRomImage rom = {};
    if (!msx_media_analyze_rom(&rom, romData, romLen)) {
        printf("[MSX] ROM analyze failed, size=%u\n", static_cast<unsigned>(romLen));
        msx_show_launch_error("MSX ROM ERROR", "Unsupported .rom image", "Expect 8 KB aligned cartridge data");
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }

    MsxBiosSearchConfig biosSearch = {};
    biosSearch.requestedMode = configuredMode;
    biosSearch.genericBiosPath = msx_config_get_bios_path();
    biosSearch.msx1BiosPath = msx_config_get_msx1_bios_path();
    biosSearch.msx2BiosPath = msx_config_get_msx2_bios_path();
    biosSearch.msx2SubRomPath = msx_config_get_msx2_subrom_path();

    MsxBiosBundle bios = {};
    if (!msx_media_load_bios_bundle(&bios, &biosSearch)) {
        printf("[MSX] BIOS load failed: %s\n", bios.message);
        msx_show_bios_reference_help(configuredMode, &bios);
        msx_media_release_bios_bundle(&bios);
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }

    printf("[MSX] BIOS bundle ready: %s\n", msx_media_bios_target_label(bios.target));

    msx_display_init();
    msx_input_init();
    msx_input_set_basic_keyboard_enabled(false);

#if MSX_AUDIO_ENABLED
    const uint32_t coreAudioSampleRate = kMsxSkeletonSampleRate;
#else
    const uint32_t coreAudioSampleRate = 0u;
#endif

    printf("[MSX] core init begin\n");

    MsxCoreState core = {};
    if (!msx_core_init(&core, &rom, &bios, romName, coreAudioSampleRate)) {
        printf("[MSX] core init failed\n");
        msx_show_launch_error("MSX START ERROR", "Core init failed", "Check ROM and BIOS set");
        msx_sound_shutdown();
        msx_media_release_bios_bundle(&bios);
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }
    msx_input_set_runtime_machine_mode(core.machineMode);

    bool audioInitOk = false;
#if MSX_AUDIO_ENABLED
    printf("[MSX] audio init begin\n");
    audioInitOk = msx_sound_init(kMsxSkeletonSampleRate, kMsxSkeletonChannels);
    printf("[MSX] audio init %s\n", audioInitOk ? "ok" : "failed");
#else
    printf("[MSX] audio init skipped (build disabled)\n");
#endif

    printf("[MSX] core init ok\n");
    printf("[MSX] entering main loop\n");
    MSX_BOOT_LOG("[MSX] boot=%s pc=%04X size=%u mapper=%s bios=%s machine=%s\n",
                core.directBoot ? "cart" : "bios",
                core.bootPc,
                static_cast<unsigned>(rom.size),
                msx_media_cartridge_type_label(rom.cartridgeType),
                msx_media_bios_target_label(bios.target),
                msx_config_machine_mode_label(core.machineMode));

    const uint32_t frameUs = static_cast<uint32_t>(std::lround(1000000.0 / kMsxSkeletonFps));
    uint64_t nextFrameUs = esp_timer_get_time();
    uint32_t frameCount = 0;
    uint32_t lastLogMs = millis();
    MsxRuntimeTimingWindow timingWindow = {};
    MsxFpsOverlayWindow fpsOverlay = {};
    msx_runtime_reset_fps_overlay(&fpsOverlay, lastLogMs);
    msx_video_set_fps_overlay_value(0u);
    bool quitRequested = false;

    while (!quitRequested) {
        MsxInputState input = {};
        msx_input_poll(&input);
        if (input.quitRequested) {
            quitRequested = true;
            break;
        }

        if (input.toggleViewRequested) {
            msx_apply_runtime_view_toggle(&core, useExternal);
        }

        if (msx_input_get_save_requested()) {
            msx_handle_save_state(&core, romName, useExternal, sd);
        }

        if (msx_input_get_load_requested()) {
            msx_handle_load_state(&core, romName, useExternal, sd);
        }

        const bool menuPaused = input.menuVisible;
        msx_sound_set_paused(menuPaused);
        msx_core_handle_input(&core, &input);

        if (menuPaused) {
            msx_core_drain_audio(&core, nullptr, 0u);
        } else {
            size_t audioMixCapacity = 0;
            int16_t* audioMix = msx_sound_begin_mix(&audioMixCapacity);
            msx_core_step_frame(&core);
            msx_runtime_timing_add(&timingWindow, &core);
            const size_t audioSampleCount = msx_core_drain_audio(&core, audioMix, audioMixCapacity);
            msx_sound_end_mix(audioSampleCount);
        }

        char cartLine[48];
        char machineLine[48];
        char biosLine[96];
        char audioLine[48];

        const MsxAudioHookState& audioState = msx_sound_get_state();

        std::snprintf(cartLine,
                      sizeof(cartLine),
                      "CART: %s %luK %s",
                      msx_media_cartridge_type_label(rom.cartridgeType),
                      static_cast<unsigned long>(rom.size / 1024u),
                      core.directBoot ? "DIRECT" : "BIOS");
        std::snprintf(machineLine,
                      sizeof(machineLine),
                      "MACHINE: %s -> %s",
                      msx_config_machine_mode_label(configuredMode),
                      msx_media_bios_target_label(core.biosTarget));
        msx_format_bios_status_line(bios, biosLine, sizeof(biosLine));

        if (!audioState.compiledIn) {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: build OFF");
        } else if (menuPaused) {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: paused");
        } else if (!audioState.enabled) {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: init OFF");
        } else if (audioState.streamSeen) {
            std::snprintf(audioLine,
                          sizeof(audioLine),
                          "AUDIO: %lu Hz Q%u",
                          static_cast<unsigned long>(audioState.sampleRate),
                          static_cast<unsigned>(audioState.queuedBlocks));
        } else {
            std::snprintf(audioLine,
                          sizeof(audioLine),
                          "AUDIO: ready %lu Hz",
                          static_cast<unsigned long>(audioState.sampleRate));
        }

        MsxDisplayStatus status = {};
        status.romName = core.romName;
        status.coreLine = core.statusText;
        status.cartLine = cartLine;
        status.machineLine = machineLine;
        status.biosLine = biosLine;
        status.audioLine = audioLine;
        status.frameCounter = core.frameCounter;

        msx_display_submit_frame(&core.displayFrame, &status);

        frameCount++;
        const uint32_t nowMs = millis();
        msx_runtime_update_fps_overlay(&fpsOverlay, menuPaused, nowMs);
        if (MSX_RUN_LOG_ENABLED && (nowMs - lastLogMs >= 1000)) {
            msx_runtime_log_summary(&core,
                                    &audioState,
                                    frameCount,
                                    nowMs - lastLogMs,
                                    &timingWindow);
            frameCount = 0;
            lastLogMs = nowMs;
        }

        nextFrameUs += frameUs;
        const int64_t nowUs = static_cast<int64_t>(esp_timer_get_time());
        const int64_t lateness = nowUs - static_cast<int64_t>(nextFrameUs);

        if (lateness > static_cast<int64_t>(frameUs)) {
            nextFrameUs = static_cast<uint64_t>(nowUs);
            taskYIELD();
            continue;
        }

        if (lateness < 0) {
            share::sleep_until_us(nextFrameUs);
        } else {
            taskYIELD();
        }
    }

    msx_core_shutdown(&core);
    msx_sound_shutdown();
    msx_media_release_bios_bundle(&bios);
    msx_display_shutdown();

    if (quitRequested) {
        msx_request_quit_to_launcher();
    }
}

// ---------------------------------------------------------------------------

void run_msx_disk(const uint8_t* dskData, size_t dskLen, const char* dskName, SdService& sd)
{
    std::printf("[MSX] launch dsk: name=%s size=%u\n",
                dskName && dskName[0] != '\0' ? dskName : "(unnamed)",
                static_cast<unsigned>(dskLen));

    const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
    MsxViewModeOverrideGuard viewModeGuard;
    msx_show_launch_controls_panel(
        useExternal,
        useExternal ? "MSX DISK EXT TFT" : "MSX DISK",
        dskName,
        "MSX DISK"
    );

    msx_config_load_internal_view_mode();
    msx_config_load_performance_mode();
    msx_config_load_fps_overlay_enabled();
    msx_config_load_frameskip_mode();
    viewModeGuard.configureForTarget(useExternal);
    const MsxMachineMode configuredMode = msx_config_load_machine_mode();
    msx_config_load_bios_path();
    msx_config_load_msx1_bios_path();
    msx_config_load_msx2_bios_path();
    msx_config_load_msx2_subrom_path();

    MsxBiosSearchConfig biosSearch = {};
    biosSearch.requestedMode    = configuredMode;
    biosSearch.genericBiosPath  = msx_config_get_bios_path();
    biosSearch.msx1BiosPath     = msx_config_get_msx1_bios_path();
    biosSearch.msx2BiosPath     = msx_config_get_msx2_bios_path();
    biosSearch.msx2SubRomPath   = msx_config_get_msx2_subrom_path();

    MsxBiosBundle bios = {};
    if (!msx_media_load_bios_bundle(&bios, &biosSearch)) {
        printf("[MSX] BIOS load failed: %s\n", bios.message);
        msx_show_bios_reference_help(configuredMode, &bios);
    msx_media_release_bios_bundle(&bios);
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }

    printf("[MSX] BIOS bundle ready: %s\n", msx_media_bios_target_label(bios.target));

#if MSX_AUDIO_ENABLED
    const uint32_t coreAudioSampleRate = kMsxSkeletonSampleRate;
#else
    const uint32_t coreAudioSampleRate = 0u;
#endif

    size_t diskRomSize = 0;
    uint8_t* diskRomData = msx_load_bios_file("DISK.ROM", 16384u, &diskRomSize);
    if (diskRomData) {
        msx_disk_apply_rom_patches(diskRomData, diskRomSize);
    } else {
        printf("[MSX] DISK.ROM not found; disk boot will fall back to BIOS only\n");
    }

    msx_display_init();
    msx_input_init();
    msx_input_set_basic_keyboard_enabled(false);

    printf("[MSX] core init_disk begin\n");
    MsxCoreState core = {};
    if (!msx_core_init_disk(&core, &bios,
                            diskRomData, diskRomSize,
                            dskData, dskLen,
                            dskName,
                            coreAudioSampleRate)) {
        printf("[MSX] core init_disk failed\n");
        msx_show_launch_error("MSX DISK ERROR", "Core init failed", "Check BIOS on SD");
        msx_sound_shutdown();
        if (diskRomData) {
            heap_caps_free(diskRomData);
        }
        msx_media_release_bios_bundle(&bios);
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }
    msx_input_set_runtime_machine_mode(core.machineMode);

    bool audioInitOk = false;
#if MSX_AUDIO_ENABLED
    printf("[MSX] audio init begin\n");
    audioInitOk = msx_sound_init(kMsxSkeletonSampleRate, kMsxSkeletonChannels);
    printf("[MSX] audio init %s\n", audioInitOk ? "ok" : "failed");
#else
    printf("[MSX] audio init skipped (build disabled)\n");
#endif

    printf("[MSX] entering disk main loop\n");

    const uint32_t frameUs = static_cast<uint32_t>(std::lround(1000000.0 / kMsxSkeletonFps));
    uint64_t nextFrameUs = esp_timer_get_time();
    bool quitRequested = false;
    uint32_t frameCount = 0;
    uint32_t lastLogMs = millis();
    MsxRuntimeTimingWindow timingWindow = {};
    MsxFpsOverlayWindow fpsOverlay = {};
    msx_runtime_reset_fps_overlay(&fpsOverlay, lastLogMs);
    msx_video_set_fps_overlay_value(0u);

    while (!quitRequested) {
        MsxInputState input = {};
        msx_input_poll(&input);
        if (input.quitRequested) {
            quitRequested = true;
            break;
        }

        if (input.toggleViewRequested) {
            msx_apply_runtime_view_toggle(&core, useExternal);
        }

        if (msx_input_get_save_requested()) {
            msx_handle_save_state(&core, dskName, useExternal, sd);
        }

        if (msx_input_get_load_requested()) {
            msx_handle_load_state(&core, dskName, useExternal, sd);
        }

        const bool menuPaused = input.menuVisible;
        msx_sound_set_paused(menuPaused);
        msx_core_handle_input(&core, &input);
        if (menuPaused) {
            msx_core_drain_audio(&core, nullptr, 0u);
        } else {
            size_t audioMixCapacity = 0;
            int16_t* audioMix = msx_sound_begin_mix(&audioMixCapacity);
            msx_core_step_frame(&core);
            msx_runtime_timing_add(&timingWindow, &core);
            const size_t audioSampleCount = msx_core_drain_audio(&core, audioMix, audioMixCapacity);
            msx_sound_end_mix(audioSampleCount);
        }

        char modeLine[48];
        char machineLine[48];
        char biosLine[96];
        char audioLine[48];

        const MsxAudioHookState& audioState = msx_sound_get_state();

        std::snprintf(modeLine,
                      sizeof(modeLine),
                      "DISK: %s",
                      dskLen > 0u ? msx_file_label(dskName) : "(no disk)");
        std::snprintf(machineLine,
                      sizeof(machineLine),
                      "MACHINE: %s -> %s",
                      msx_config_machine_mode_label(configuredMode),
                      msx_media_bios_target_label(core.biosTarget));
        msx_format_bios_status_line(bios, biosLine, sizeof(biosLine));

        if (!audioState.compiledIn) {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: build OFF");
        } else if (menuPaused) {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: paused");
        } else if (!audioState.enabled) {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: init OFF");
        } else if (audioState.streamSeen) {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: %lu Hz Q%u",
                          static_cast<unsigned long>(audioState.sampleRate),
                          static_cast<unsigned>(audioState.queuedBlocks));
        } else {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: ready %lu Hz",
                          static_cast<unsigned long>(audioState.sampleRate));
        }

        MsxDisplayStatus status = {};
        status.romName     = core.romName;
        status.coreLine    = core.statusText;
        status.cartLine    = modeLine;
        status.machineLine = machineLine;
        status.biosLine    = biosLine;
        status.audioLine   = audioLine;
        status.frameCounter = core.frameCounter;

        msx_display_submit_frame(&core.displayFrame, &status);

        frameCount++;
        const uint32_t nowMs = millis();
        msx_runtime_update_fps_overlay(&fpsOverlay, menuPaused, nowMs);
        if (MSX_RUN_LOG_ENABLED && (nowMs - lastLogMs >= 1000)) {
            msx_runtime_log_summary(&core,
                                    &audioState,
                                    frameCount,
                                    nowMs - lastLogMs,
                                    &timingWindow,
                                    "DISK");
            frameCount = 0;
            lastLogMs = nowMs;
        }

        nextFrameUs += frameUs;
        const int64_t nowUs   = static_cast<int64_t>(esp_timer_get_time());
        const int64_t lateness = nowUs - static_cast<int64_t>(nextFrameUs);

        if (lateness > static_cast<int64_t>(frameUs)) {
            nextFrameUs = static_cast<uint64_t>(nowUs);
            taskYIELD();
            continue;
        }

        if (lateness < 0) {
            share::sleep_until_us(nextFrameUs);
        } else {
            taskYIELD();
        }
    }

    msx_core_shutdown(&core);
    if (diskRomData) {
        heap_caps_free(diskRomData);
    }
    msx_sound_shutdown();
    msx_media_release_bios_bundle(&bios);
    msx_display_shutdown();

    if (quitRequested) {
        msx_request_quit_to_launcher();
    }
}

// ---------------------------------------------------------------------------

void run_msx_basic(const char* name, SdService& sd)
{
    const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
    MsxViewModeOverrideGuard viewModeGuard;
    msx_show_launch_controls_panel(
        useExternal,
        useExternal ? "MSX BASIC EXT TFT" : "MSX BASIC",
        name,
        "MSX BASIC"
    );

    msx_config_load_internal_view_mode();
    msx_config_load_performance_mode();
    msx_config_load_fps_overlay_enabled();
    msx_config_load_frameskip_mode();
    viewModeGuard.configureForTarget(useExternal);
    const MsxMachineMode configuredMode = msx_config_load_machine_mode();
    msx_config_load_bios_path();
    msx_config_load_msx1_bios_path();
    msx_config_load_msx2_bios_path();
    msx_config_load_msx2_subrom_path();

    MsxBiosSearchConfig biosSearch = {};
    biosSearch.requestedMode    = configuredMode;
    biosSearch.genericBiosPath  = msx_config_get_bios_path();
    biosSearch.msx1BiosPath     = msx_config_get_msx1_bios_path();
    biosSearch.msx2BiosPath     = msx_config_get_msx2_bios_path();
    biosSearch.msx2SubRomPath   = msx_config_get_msx2_subrom_path();

    MsxBiosBundle bios = {};
    if (!msx_media_load_bios_bundle(&bios, &biosSearch)) {
        printf("[MSX] BIOS load failed: %s\n", bios.message);
        msx_show_bios_reference_help(configuredMode, &bios);
        msx_media_release_bios_bundle(&bios);
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }

    printf("[MSX] BIOS bundle ready: %s\n", msx_media_bios_target_label(bios.target));

    msx_display_init();
    msx_input_init();
    msx_input_set_basic_keyboard_enabled(true);

#if MSX_AUDIO_ENABLED
    const uint32_t coreAudioSampleRate = kMsxSkeletonSampleRate;
#else
    const uint32_t coreAudioSampleRate = 0u;
#endif

    printf("[MSX] core init_basic begin\n");
    MsxCoreState core = {};
    if (!msx_core_init_basic(&core, &bios, name, coreAudioSampleRate)) {
        printf("[MSX] core init_basic failed\n");
        msx_show_launch_error("MSX BASIC ERROR", "Core init failed", "Check BIOS on SD");
        msx_sound_shutdown();
        msx_media_release_bios_bundle(&bios);
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }
    msx_input_set_runtime_machine_mode(core.machineMode);

    bool audioInitOk = false;
#if MSX_AUDIO_ENABLED
    printf("[MSX] audio init begin\n");
    audioInitOk = msx_sound_init(kMsxSkeletonSampleRate, kMsxSkeletonChannels);
    printf("[MSX] audio init %s\n", audioInitOk ? "ok" : "failed");
#else
    printf("[MSX] audio init skipped (build disabled)\n");
#endif

    printf("[MSX] entering BASIC main loop\n");

    const uint32_t frameUs = static_cast<uint32_t>(std::lround(1000000.0 / kMsxSkeletonFps));
    uint64_t nextFrameUs = esp_timer_get_time();
    bool quitRequested = false;
    uint32_t frameCount = 0;
    uint32_t lastLogMs = millis();
    MsxRuntimeTimingWindow timingWindow = {};
    MsxFpsOverlayWindow fpsOverlay = {};
    msx_runtime_reset_fps_overlay(&fpsOverlay, lastLogMs);
    msx_video_set_fps_overlay_value(0u);

    while (!quitRequested) {
        MsxInputState input = {};
        msx_input_poll(&input);
        if (input.quitRequested) {
            quitRequested = true;
            break;
        }

        if (input.toggleViewRequested) {
            msx_apply_runtime_view_toggle(&core, useExternal);
        }

        if (msx_input_get_save_requested()) {
            msx_handle_save_state(&core, name, useExternal, sd);
        }

        if (msx_input_get_load_requested()) {
            msx_handle_load_state(&core, name, useExternal, sd);
        }

        const bool menuPaused = input.menuVisible;
        msx_sound_set_paused(menuPaused);
        msx_core_handle_input(&core, &input);
        if (menuPaused) {
            msx_core_drain_audio(&core, nullptr, 0u);
        } else {
            size_t audioMixCapacity = 0;
            int16_t* audioMix = msx_sound_begin_mix(&audioMixCapacity);
            msx_core_step_frame(&core);
            msx_runtime_timing_add(&timingWindow, &core);
            const size_t audioSampleCount = msx_core_drain_audio(&core, audioMix, audioMixCapacity);
            msx_sound_end_mix(audioSampleCount);
        }

        char machineLine[48];
        char biosLine[96];

        std::snprintf(machineLine, sizeof(machineLine),
                      "MACHINE: %s -> %s",
                      msx_config_machine_mode_label(configuredMode),
                      msx_media_bios_target_label(core.biosTarget));
        msx_format_bios_status_line(bios, biosLine, sizeof(biosLine));

        MsxDisplayStatus status = {};
        status.romName      = core.romName;
        status.coreLine     = core.statusText;
        status.cartLine     = "BASIC (no cart)";
        status.machineLine  = machineLine;
        status.biosLine     = biosLine;
        const MsxAudioHookState& audioState = msx_sound_get_state();
        if (!audioState.compiledIn) {
            status.audioLine = "AUDIO: build OFF";
        } else if (menuPaused) {
            status.audioLine = "AUDIO: paused";
        } else if (!audioState.enabled) {
            status.audioLine = "AUDIO: init OFF";
        } else if (audioState.streamSeen) {
            static char basicAudioLine[48];
            std::snprintf(basicAudioLine,
                          sizeof(basicAudioLine),
                          "AUDIO: %lu Hz Q%u",
                          static_cast<unsigned long>(audioState.sampleRate),
                          static_cast<unsigned>(audioState.queuedBlocks));
            status.audioLine = basicAudioLine;
        } else {
            static char basicAudioLine[48];
            std::snprintf(basicAudioLine,
                          sizeof(basicAudioLine),
                          "AUDIO: ready %lu Hz",
                          static_cast<unsigned long>(audioState.sampleRate));
            status.audioLine = basicAudioLine;
        }
        status.frameCounter = core.frameCounter;

        msx_display_submit_frame(&core.displayFrame, &status);

        frameCount++;
        const uint32_t nowMs = millis();
        msx_runtime_update_fps_overlay(&fpsOverlay, menuPaused, nowMs);
        if (MSX_RUN_LOG_ENABLED && (nowMs - lastLogMs >= 1000)) {
            msx_runtime_log_summary(&core,
                                    &audioState,
                                    frameCount,
                                    nowMs - lastLogMs,
                                    &timingWindow,
                                    "BASIC");
            frameCount = 0;
            lastLogMs = nowMs;
        }

        nextFrameUs += frameUs;
        const int64_t nowUs = static_cast<int64_t>(esp_timer_get_time());
        const int64_t lateness = nowUs - static_cast<int64_t>(nextFrameUs);

        if (lateness > static_cast<int64_t>(frameUs)) {
            nextFrameUs = static_cast<uint64_t>(nowUs);
            taskYIELD();
            continue;
        }

        if (lateness < 0) {
            share::sleep_until_us(nextFrameUs);
        } else {
            taskYIELD();
        }
    }

    msx_core_shutdown(&core);
    msx_sound_shutdown();

    msx_media_release_bios_bundle(&bios);
    msx_display_shutdown();

    if (quitRequested) {
        msx_request_quit_to_launcher();
    }
}

// ---------------------------------------------------------------------------

void run_msx_cas(const uint8_t* casData, size_t casLen, const char* casName, SdService& sd)
{
    std::printf("[MSX] launch cas: name=%s size=%u\n",
                casName && casName[0] != '\0' ? casName : "(unnamed)",
                static_cast<unsigned>(casLen));

    const bool useExternal = (g_emu_display_target == EMU_DISPLAY_EXTERNAL);
    MsxViewModeOverrideGuard viewModeGuard;
    msx_show_launch_controls_panel(
        useExternal,
        "MSX CAS",
        casName,
        "MSX CAS"
    );

    msx_config_load_internal_view_mode();
    msx_config_load_performance_mode();
    msx_config_load_fps_overlay_enabled();
    msx_config_load_frameskip_mode();
    viewModeGuard.configureForTarget(useExternal);
    const MsxMachineMode configuredMode = msx_config_load_machine_mode();
    msx_config_load_bios_path();
    msx_config_load_msx1_bios_path();
    msx_config_load_msx2_bios_path();
    msx_config_load_msx2_subrom_path();

    MsxBiosSearchConfig biosSearch = {};
    biosSearch.requestedMode    = configuredMode;
    biosSearch.genericBiosPath  = msx_config_get_bios_path();
    biosSearch.msx1BiosPath     = msx_config_get_msx1_bios_path();
    biosSearch.msx2BiosPath     = msx_config_get_msx2_bios_path();
    biosSearch.msx2SubRomPath   = msx_config_get_msx2_subrom_path();

    MsxBiosBundle bios = {};
    if (!msx_media_load_bios_bundle(&bios, &biosSearch)) {
        printf("[MSX] BIOS load failed: %s\n", bios.message);
        msx_show_bios_reference_help(configuredMode, &bios);
        msx_media_release_bios_bundle(&bios);
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }

    printf("[MSX] BIOS bundle ready: %s\n", msx_media_bios_target_label(bios.target));

    msx_display_init();
    msx_input_init();
    msx_input_set_basic_keyboard_enabled(true);
    msx_input_set_cas_change_available(true);

#if MSX_AUDIO_ENABLED
    const uint32_t coreAudioSampleRate = kMsxSkeletonSampleRate;
#else
    const uint32_t coreAudioSampleRate = 0u;
#endif

    printf("[MSX] core init_cas begin\n");
    MsxCoreState core = {};
    if (!msx_core_init_cas(&core, &bios, casData, casLen, casName, coreAudioSampleRate)) {
        printf("[MSX] core init_cas failed\n");
        msx_show_launch_error("MSX CAS ERROR", "Core init failed", "Check BIOS on SD");
        msx_sound_shutdown();
        msx_media_release_bios_bundle(&bios);
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }
    msx_input_set_runtime_machine_mode(core.machineMode);

    bool audioInitOk = false;
#if MSX_AUDIO_ENABLED
    printf("[MSX] audio init begin\n");
    audioInitOk = msx_sound_init(kMsxSkeletonSampleRate, kMsxSkeletonChannels);
    printf("[MSX] audio init %s\n", audioInitOk ? "ok" : "failed");
#else
    printf("[MSX] audio init skipped (build disabled)\n");
#endif

    printf("[MSX] entering CAS main loop\n");

    std::string currentCasPath = msx_initial_cas_display_path(casName);
    if (currentCasPath.empty()) {
        currentCasPath = casName ? std::string(casName) : std::string("MSX CAS");
    }
    std::vector<uint8_t> runtimeCasBuffer;

    const uint32_t frameUs = static_cast<uint32_t>(std::lround(1000000.0 / kMsxSkeletonFps));
    uint64_t nextFrameUs = esp_timer_get_time();
    bool quitRequested = false;
    uint32_t frameCount = 0;
    uint32_t lastLogMs = millis();
    MsxRuntimeTimingWindow timingWindow = {};
    MsxFpsOverlayWindow fpsOverlay = {};
    msx_runtime_reset_fps_overlay(&fpsOverlay, lastLogMs);
    msx_video_set_fps_overlay_value(0u);

    while (!quitRequested) {
        MsxInputState input = {};
        msx_input_poll(&input);
        if (input.quitRequested) {
            quitRequested = true;
            break;
        }

        if (input.toggleViewRequested) {
            msx_apply_runtime_view_toggle(&core, useExternal);
        }

        if (msx_input_get_change_cas_requested()) {
            msx_sound_set_paused(true);
            msx_handle_change_cas(&core, currentCasPath, runtimeCasBuffer, useExternal, sd);
            if (useExternal) {
                msx_show_launch_controls_panel(
                    true,
                    "MSX CAS",
                    currentCasPath.c_str(),
                    "MSX CAS"
                );
            }
            nextFrameUs = esp_timer_get_time();
            continue;
        }

        if (msx_input_get_save_requested()) {
            msx_handle_save_state(&core, currentCasPath.c_str(), useExternal, sd);
        }

        if (msx_input_get_load_requested()) {
            msx_handle_load_state(&core, currentCasPath.c_str(), useExternal, sd);
        }

        const bool menuPaused = input.menuVisible;
        msx_sound_set_paused(menuPaused);
        msx_core_handle_input(&core, &input);
        if (menuPaused) {
            msx_core_drain_audio(&core, nullptr, 0u);
        } else {
            size_t audioMixCapacity = 0;
            int16_t* audioMix = msx_sound_begin_mix(&audioMixCapacity);
            msx_core_step_frame(&core);
            msx_runtime_timing_add(&timingWindow, &core);
            const size_t audioSampleCount = msx_core_drain_audio(&core, audioMix, audioMixCapacity);
            msx_sound_end_mix(audioSampleCount);
        }

        char casLine[48];
        char machineLine[48];
        char biosLine[96];

        std::snprintf(casLine, sizeof(casLine),
                      "CAS: %s",
                      core.cas.ready ? msx_file_label(currentCasPath.c_str()) : "(no tape)");
        std::snprintf(machineLine, sizeof(machineLine),
                      "MACHINE: %s -> %s",
                      msx_config_machine_mode_label(configuredMode),
                      msx_media_bios_target_label(core.biosTarget));
        msx_format_bios_status_line(bios, biosLine, sizeof(biosLine));

        const MsxAudioHookState& audioState = msx_sound_get_state();
        static char casAudioLine[48];
        if (!audioState.compiledIn) {
            std::snprintf(casAudioLine, sizeof(casAudioLine), "AUDIO: build OFF");
        } else if (menuPaused) {
            std::snprintf(casAudioLine, sizeof(casAudioLine), "AUDIO: paused");
        } else if (!audioState.enabled) {
            std::snprintf(casAudioLine, sizeof(casAudioLine), "AUDIO: init OFF");
        } else if (audioState.streamSeen) {
            std::snprintf(casAudioLine, sizeof(casAudioLine),
                          "AUDIO: %lu Hz Q%u",
                          static_cast<unsigned long>(audioState.sampleRate),
                          static_cast<unsigned>(audioState.queuedBlocks));
        } else {
            std::snprintf(casAudioLine, sizeof(casAudioLine),
                          "AUDIO: ready %lu Hz",
                          static_cast<unsigned long>(audioState.sampleRate));
        }

        MsxDisplayStatus status = {};
        status.romName     = core.romName;
        status.coreLine    = core.statusText;
        status.cartLine    = casLine;
        status.machineLine = machineLine;
        status.biosLine    = biosLine;
        status.audioLine   = casAudioLine;
        status.frameCounter = core.frameCounter;

        msx_display_submit_frame(&core.displayFrame, &status);

        frameCount++;
        const uint32_t nowMs = millis();
        msx_runtime_update_fps_overlay(&fpsOverlay, menuPaused, nowMs);
        if (MSX_RUN_LOG_ENABLED && (nowMs - lastLogMs >= 1000)) {
            msx_runtime_log_summary(&core,
                                    &audioState,
                                    frameCount,
                                    nowMs - lastLogMs,
                                    &timingWindow,
                                    "CAS");
            frameCount = 0;
            lastLogMs = nowMs;
        }

        nextFrameUs += frameUs;
        const int64_t nowUs    = static_cast<int64_t>(esp_timer_get_time());
        const int64_t lateness = nowUs - static_cast<int64_t>(nextFrameUs);

        if (lateness > static_cast<int64_t>(frameUs)) {
            nextFrameUs = static_cast<uint64_t>(nowUs);
            taskYIELD();
            continue;
        }

        if (lateness < 0) {
            share::sleep_until_us(nextFrameUs);
        } else {
            taskYIELD();
        }
    }

    msx_core_shutdown(&core);
    msx_sound_shutdown();
    msx_media_release_bios_bundle(&bios);
    msx_display_shutdown();

    if (quitRequested) {
        msx_request_quit_to_launcher();
    }
}
