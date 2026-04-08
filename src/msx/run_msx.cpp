#include "run_msx.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <Preferences.h>
#include <SD.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cardputer/CardputerInput.h"
#include "cardputer/CardputerView.h"
#include "core/msx_core.h"
#include "core/msx_disk.h"
#include "esp_timer.h"
#include "msx_config.h"
#include "msx_display.h"
#include "msx_input.h"
#include "msx_media.h"
#include "msx_sound.h"
#include "share/emu_controls.h"
#include "share/game_save.h"
#include "share/utils.h"

#ifndef MSX_RUN_LOG_ENABLED
#define MSX_RUN_LOG_ENABLED 0
#endif

#if MSX_RUN_LOG_ENABLED
#define MSX_RUN_LOG(...) std::printf(__VA_ARGS__)
#else
#define MSX_RUN_LOG(...) do { } while (0)
#endif

namespace {

struct MsxBiosReferenceEntry {
    const char* name;
    const char* md5;
};

constexpr uint32_t kMsxSkeletonSampleRate = 22050;
constexpr uint8_t kMsxSkeletonChannels = 1;
constexpr double kMsxSkeletonFps = 60.0;
constexpr const char* kMsxBiosDirPrimary = "/sd/bios/msx/";
constexpr const char* kMsxBiosSdDirPrimary = "bios/msx/";
constexpr const char* kMsxBiosSdDirFallback = "msx/";
constexpr const char* kMsxBiosDirFallback = "/sd/msx/";

static const char* kMsxBiosReferenceTable =
    "MSX.ROM      364a1a579fe5cb8dba54519bcfcdac0d\n"
    "MSX2.ROM     ec3a01c91f24fbddcbcab0ad301bc9ef\n"
    "MSX2EXT.ROM  2183c2aff17cf4297bdb496de78c2e8a\n"
    "MSX2P.ROM    847cc025ffae665487940ff2639540e5\n"
    "MSX2PEXT.ROM 7c8243c71d8f143b2531f01afa6a05dc\n"
    "DISK.ROM     80dcd1ad1a4cf65d64b7ba10504e8190\n"
    "MSXDOS2.ROM  6418d091cd6907bbcf940324339e43bb\n"
    "FMPAC.ROM    6f69cc8b5ed761b03afd78000dfb0e19\n";

constexpr MsxBiosReferenceEntry kMsxBiosReferences[] = {
    {"MSX.ROM",      "364a1a579fe5cb8dba54519bcfcdac0d"},
    {"MSX2.ROM",     "ec3a01c91f24fbddcbcab0ad301bc9ef"},
    {"MSX2EXT.ROM",  "2183c2aff17cf4297bdb496de78c2e8a"},
    {"MSX2P.ROM",    "847cc025ffae665487940ff2639540e5"},
    {"MSX2PEXT.ROM", "7c8243c71d8f143b2531f01afa6a05dc"},
    {"DISK.ROM",     "80dcd1ad1a4cf65d64b7ba10504e8190"},
    {"MSXDOS2.ROM",  "6418d091cd6907bbcf940324339e43bb"},
    {"FMPAC.ROM",    "6f69cc8b5ed761b03afd78000dfb0e19"},
};

static uint8_t s_msx_disk_rom_static[16384];
static bool s_msx_disk_rom_static_used = false;

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

    const char* introLines[7] = {
        bios && bios->message[0] != '\0' ? bios->message : "Missing BIOS files.",
        "Place BIOS on SD in:",
        kMsxBiosSdDirPrimary,
        kMsxBiosSdDirFallback,
        nullptr,
        nullptr,
        nullptr,
    };

    switch (configuredMode) {
        case MsxMachineMode::MSX1:
            introLines[4] = "Need now: MSX.ROM";
            break;
        case MsxMachineMode::MSX2:
            introLines[4] = "Need now: MSX2.ROM";
            introLines[5] = "Need now: MSX2EXT.ROM";
            break;
        case MsxMachineMode::Auto:
        default:
            introLines[4] = "AUTO tries MSX1 first";
            introLines[5] = "Fallback: MSX2.ROM + MSX2EXT.ROM";
            introLines[6] = "Need: MSX.ROM";
            break;
    }

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
        if (!buf && std::strcmp(filename, "DISK.ROM") == 0 && fileSize == 16384 && !s_msx_disk_rom_static_used) {
            buf = s_msx_disk_rom_static;
            s_msx_disk_rom_static_used = true;
            std::printf("[MSX] %s: using static fallback buffer (%u B)\n", openPath, static_cast<unsigned>(fileSize));
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

} // namespace

void run_msx(const uint8_t* romData, size_t romLen, const char* romName)
{
    {
        CardputerView display;
        display.initialize();
        display.topBar("MSX ON INTERNAL LCD", false, false);
        display.showControlBindings(
            share::emuControlActionLabels(share::EmuProfile::MSX),
            share::emuControlKeyLabels(share::EmuProfile::MSX),
            "GO / HOLD ESC = QUIT"
        );
    }

    msx_config_load_internal_view_mode();
    const MsxMachineMode configuredMode = msx_config_load_machine_mode();
    msx_config_load_bios_path();
    msx_config_load_msx1_bios_path();
    msx_config_load_msx2_bios_path();
    msx_config_load_msx2_subrom_path();

    msx_display_init();
    msx_input_init();

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
    MSX_RUN_LOG("[MSX] boot=%s pc=%04X size=%u mapper=%s bios=%s machine=%s\n",
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
    bool quitRequested = false;

    while (!quitRequested) {
        MsxInputState input = {};
        msx_input_poll(&input);
        if (input.quitRequested) {
            quitRequested = true;
            break;
        }

        if (input.toggleViewRequested) {
            msx_config_toggle_internal_view_mode();
        }

        size_t audioMixCapacity = 0;
        int16_t* audioMix = msx_sound_begin_mix(&audioMixCapacity);
        (void)audioMix;
        (void)audioMixCapacity;

        msx_core_handle_input(&core, &input);
        msx_core_step_frame(&core);
        const size_t audioSampleCount = msx_core_drain_audio(&core, audioMix, audioMixCapacity);
        msx_sound_end_mix(audioSampleCount);

        char cartLine[48];
        char machineLine[48];
        char biosLine[64];
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
        if (bios.target == MsxBiosTarget::MSX2) {
            std::snprintf(biosLine,
                          sizeof(biosLine),
                          "BIOS: %s + %s",
                          msx_file_label(bios.mainRom.path),
                          msx_file_label(bios.subRom.path));
        } else {
            std::snprintf(biosLine,
                          sizeof(biosLine),
                          "BIOS: %s",
                          msx_file_label(bios.mainRom.path));
        }

        if (!audioState.compiledIn) {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: build OFF");
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
        if (MSX_RUN_LOG_ENABLED && (nowMs - lastLogMs >= 1000)) {
            const float fps = (frameCount * 1000.0f) / static_cast<float>(nowMs - lastLogMs);
            MSX_RUN_LOG("[MSX] FPS %.1f | HEAP %u | CPU %s | PC %04X | VDP %s | MACHINE %s | AUDIOQ %u\n",
                        fps,
                        esp_get_free_heap_size(),
                        msx_cpu_run_state_label(core.cpu.runState),
                        core.cpu.pc,
                        msx_vdp_mode_label(core.vdp.mode),
                        msx_media_bios_target_label(core.biosTarget),
                        static_cast<unsigned>(audioState.queuedBlocks));
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

void run_msx_disk(const uint8_t* dskData, size_t dskLen, const char* dskName)
{
    {
        CardputerView display;
        display.initialize();
        display.topBar("MSX DISK", false, false);
        display.showControlBindings(
            share::emuControlActionLabels(share::EmuProfile::MSX),
            share::emuControlKeyLabels(share::EmuProfile::MSX),
            "GO / HOLD ESC = QUIT"
        );
    }

    msx_config_load_internal_view_mode();
    const MsxMachineMode configuredMode = msx_config_load_machine_mode();
    msx_config_load_bios_path();
    msx_config_load_msx1_bios_path();
    msx_config_load_msx2_bios_path();
    msx_config_load_msx2_subrom_path();

    msx_display_init();
    msx_input_init();

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
        if (diskRomData && diskRomData != s_msx_disk_rom_static) {
            heap_caps_free(diskRomData);
        }
        s_msx_disk_rom_static_used = false;
        msx_media_release_bios_bundle(&bios);
        msx_display_shutdown();
        msx_request_quit_to_launcher();
        return;
    }

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

    while (!quitRequested) {
        MsxInputState input = {};
        msx_input_poll(&input);
        if (input.quitRequested) {
            quitRequested = true;
            break;
        }

        if (input.toggleViewRequested) {
            msx_config_toggle_internal_view_mode();
        }

        size_t audioMixCapacity = 0;
        int16_t* audioMix = msx_sound_begin_mix(&audioMixCapacity);
        (void)audioMix;
        (void)audioMixCapacity;

        msx_core_handle_input(&core, &input);
        msx_core_step_frame(&core);
        const size_t audioSampleCount = msx_core_drain_audio(&core, audioMix, audioMixCapacity);
        msx_sound_end_mix(audioSampleCount);

        char modeLine[48];
        char machineLine[48];
        char biosLine[64];
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
        if (bios.target == MsxBiosTarget::MSX2) {
            std::snprintf(biosLine, sizeof(biosLine),
                          "BIOS: %s + %s",
                          msx_file_label(bios.mainRom.path),
                          msx_file_label(bios.subRom.path));
        } else {
            std::snprintf(biosLine, sizeof(biosLine),
                          "BIOS: %s",
                          msx_file_label(bios.mainRom.path));
        }

        if (!audioState.compiledIn) {
            std::snprintf(audioLine, sizeof(audioLine), "AUDIO: build OFF");
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
    if (diskRomData && diskRomData != s_msx_disk_rom_static) {
        heap_caps_free(diskRomData);
    }
    s_msx_disk_rom_static_used = false;
    msx_sound_shutdown();
    msx_media_release_bios_bundle(&bios);
    msx_display_shutdown();

    if (quitRequested) {
        msx_request_quit_to_launcher();
    }
}

// ---------------------------------------------------------------------------

void run_msx_basic(const char* name)
{
    {
        CardputerView display;
        display.initialize();
        display.topBar("MSX BASIC", false, false);
        display.showControlBindings(
            share::emuControlActionLabels(share::EmuProfile::MSX),
            share::emuControlKeyLabels(share::EmuProfile::MSX),
            "GO / HOLD ESC = QUIT"
        );
    }

    msx_config_load_internal_view_mode();
    const MsxMachineMode configuredMode = msx_config_load_machine_mode();
    msx_config_load_bios_path();
    msx_config_load_msx1_bios_path();
    msx_config_load_msx2_bios_path();
    msx_config_load_msx2_subrom_path();

    msx_display_init();
    msx_input_init();

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

    while (!quitRequested) {
        MsxInputState input = {};
        msx_input_poll(&input);
        if (input.quitRequested) {
            quitRequested = true;
            break;
        }

        if (input.toggleViewRequested) {
            msx_config_toggle_internal_view_mode();
        }

        size_t audioMixCapacity = 0;
        int16_t* audioMix = msx_sound_begin_mix(&audioMixCapacity);
        (void)audioMix;
        (void)audioMixCapacity;

        msx_core_handle_input(&core, &input);
        msx_core_step_frame(&core);
        const size_t audioSampleCount = msx_core_drain_audio(&core, audioMix, audioMixCapacity);
        msx_sound_end_mix(audioSampleCount);

        char machineLine[48];
        char biosLine[64];

        std::snprintf(machineLine, sizeof(machineLine),
                      "MACHINE: %s -> %s",
                      msx_config_machine_mode_label(configuredMode),
                      msx_media_bios_target_label(core.biosTarget));
        if (bios.target == MsxBiosTarget::MSX2) {
            std::snprintf(biosLine, sizeof(biosLine),
                          "BIOS: %s + %s",
                          msx_file_label(bios.mainRom.path),
                          msx_file_label(bios.subRom.path));
        } else {
            std::snprintf(biosLine, sizeof(biosLine),
                          "BIOS: %s",
                          msx_file_label(bios.mainRom.path));
        }

        MsxDisplayStatus status = {};
        status.romName      = core.romName;
        status.coreLine     = core.statusText;
        status.cartLine     = "BASIC (no cart)";
        status.machineLine  = machineLine;
        status.biosLine     = biosLine;
        const MsxAudioHookState& audioState = msx_sound_get_state();
        if (!audioState.compiledIn) {
            status.audioLine = "AUDIO: build OFF";
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
