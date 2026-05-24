#pragma GCC optimize ("Os")

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <algorithm>

#include "compat/preferences_compat.h"
#include "cardputer/SdService.h"
#include "cardputer/CardputerView.h"
#include "cardputer/VerticalSelector.h" 
#include "cardputer/CardputerInput.h"

static inline std::string romSelectorBasename(const std::string& path) {
    if (path.empty()) return "";
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

static inline std::string romSelectorParent(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos || slash == 0) return "/";
    return path.substr(0, slash);
}

static inline void saveRomCursorToNvs(const std::string& dirPath, const std::string& itemName) {
    if (dirPath.empty() || itemName.empty()) return;
    Preferences prefs;
    if (!prefs.begin("cardputer_emu", false)) return;
    prefs.putString("rom_cur_dir", dirPath.c_str());
    prefs.putString("rom_cur_item", itemName.c_str());
    prefs.end();
}

static inline void loadRomCursorFromNvs(std::string& dirPath, std::string& itemName) {
    Preferences prefs;
    if (!prefs.begin("cardputer_emu", true)) {
        dirPath.clear();
        itemName.clear();
        return;
    }
    dirPath = prefs.getString("rom_cur_dir", "");
    itemName = prefs.getString("rom_cur_item", "");
    if (dirPath.empty() || itemName.empty()) {
        std::string lastGame = prefs.getString("last_game", "");
        if (!lastGame.empty()) {
            dirPath = romSelectorParent(lastGame);
            itemName = romSelectorBasename(lastGame);
        }
    }
    prefs.end();
}

static inline int findRomCursorIndex(
    const std::vector<std::string>& elements,
    const std::string& dirPath,
    const std::string& savedDir,
    const std::string& savedItem
) {
    if (elements.empty() || dirPath != savedDir || savedItem.empty()) return 0;
    auto it = std::find(elements.begin(), elements.end(), savedItem);
    if (it == elements.end()) return 0;
    return (int)std::distance(elements.begin(), it);
}

static inline void showRomConfigMenu(
    SdService& sdService,
    CardputerView& display,
    CardputerInput& input,
    const std::string& currentPath,
    std::vector<std::string>& elementNames,
    std::string& previousPath
) {
    VerticalSelector configSelector(display, input);
    const std::vector<std::string> options = {"SCAN DIR", "EXIT"};

    for (;;) {
        int action = configSelector.select("CONFIG MENU", options, true, false);
        if (action == VERTICAL_SELECTOR_G0 || action == VERTICAL_SELECTOR_BACK || action == 1) {
            return;
        }

        if (action == 0) {
            display.topBar("SCAN DIR", false, false);
            display.subMessage("Scanning...", 0);

            size_t count = 0;
            if (sdService.writeDirectoryIndex(currentPath, &count)) {
                display.subMessage("Index saved", 700);
                char msg[32];
                snprintf(msg, sizeof(msg), "%u entries", (unsigned)count);
                display.subMessage(msg, 700);
                elementNames = sdService.getCachedDirectoryElements(currentPath);
                previousPath.clear();
            } else {
                display.subMessage("Index failed", 1200);
            }
            return;
        }
    }
}

// Returns the absolute path of a .nes selected file
// - Navigates folders with verticalSelector
// - When a selected item is a nes file, returns its path

enum RomType {
    ROM_TYPE_UNKNOWN = 0,
    ROM_TYPE_NES,
    ROM_TYPE_SMS,
    ROM_TYPE_GAMEGEAR,
    ROM_TYPE_SG1000,
    ROM_TYPE_COLECO,
    ROM_TYPE_NGP,
    ROM_TYPE_GENESIS,
    ROM_TYPE_WS,
    ROM_TYPE_PCE,
    ROM_TYPE_GB,
    ROM_TYPE_LYNX,
    ROM_TYPE_SNES,
    ROM_TYPE_MSX,
    ROM_TYPE_ATARI7800,
    ROM_TYPE_ATARI2600,
    ROM_TYPE_GX4000
};

// NGP types
static constexpr uint8_t NGP  = 0; // Monochrome
static constexpr uint8_t NGPC = 1; // Color

static inline bool hasRomExt(const std::string& path) {
    if (path.size() < 3) return false;

    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) return false;

    std::string ext = path.substr(dotPos + 1);

    for (auto &ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    return (
#ifdef NES_CORE_ENABLED
            ext == "nes" ||
#endif
#ifdef SMS_CORE_ENABLED
            ext == "gg" || ext == "sms" || ext == "sg" || ext == "sc" || ext == "col" ||
#endif
#ifdef NGP_CORE_ENABLED
            ext == "ngc" || ext == "ngp" ||
#endif
#ifdef MD_CORE_ENABLED
            ext == "md" ||
#endif
#ifdef WS_CORE_ENABLED
            ext == "ws" || ext == "wsc" ||
#endif
#ifdef PCE_CORE_ENABLED
            ext == "pce" ||
#endif
#ifdef GB_CORE_ENABLED
            ext == "gb" || ext == "gbc" ||
#endif
#ifdef LYNX_CORE_ENABLED
            ext == "lnx" ||
#endif
#ifdef SNES_CORE_ENABLED
            ext == "sfc" || ext == "smc" ||
#endif
#ifdef MSX_CORE_ENABLED
            ext == "rom" || ext == "mx1" ||
#endif
#ifdef A7800_CORE_ENABLED
            ext == "a78" ||
#endif
#ifdef A2600_CORE_ENABLED
            ext == "a26" ||
#endif
#ifdef GX4000_CORE_ENABLED
            ext == "cpr" ||
#endif
            false);
}

static inline int detectNeoGeoPocketFromRom(const uint8_t* rom, size_t size, const std::string& filepath)
{
  // Header cart 0x23
  if (rom && size >= 0x24) {
    const uint8_t comp = rom[0x23];
    if (comp == NGP)  return NGP;
    if (comp == NGPC) return NGPC;
  }
  // fallback
  return NGPC;
}

static inline int detectWonderSwanFromRom(const std::string& filepath)
{
    // by extension
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos == std::string::npos) return 0;

    std::string ext = filepath.substr(dotPos + 1);

    for (auto &ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    if (ext == "ws") return 0;   // WonderSwan
    if (ext == "wsc") return 1;  // WonderSwan Color

    return 0; // default
}

RomType getRomType(const std::string& path) {
    if (path.empty()) return ROM_TYPE_UNKNOWN;

    // find last dot
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos || dotPos + 1 >= path.size())
        return ROM_TYPE_UNKNOWN;

    // Extract the extension
    std::string ext = path.substr(dotPos + 1);

    // Convert to lowercase
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

#ifdef NES_CORE_ENABLED
    if (ext == "nes") return ROM_TYPE_NES;
#endif
#ifdef SMS_CORE_ENABLED
    if (ext == "sms") return ROM_TYPE_SMS;
    if (ext == "gg")  return ROM_TYPE_GAMEGEAR;
    if (ext == "sg" || ext == "sc") return ROM_TYPE_SG1000;
    if (ext == "col") return ROM_TYPE_COLECO;
#endif
#ifdef NGP_CORE_ENABLED
    if (ext == "ngc") return ROM_TYPE_NGP;
    if (ext == "ngp") return ROM_TYPE_NGP;
#endif
#ifdef MD_CORE_ENABLED
    if (ext == "md")  return ROM_TYPE_GENESIS;
#endif
#ifdef WS_CORE_ENABLED
    if (ext == "ws" )  return ROM_TYPE_WS;
    if (ext == "wsc")  return ROM_TYPE_WS;
#endif
#ifdef PCE_CORE_ENABLED
    if (ext == "pce")  return ROM_TYPE_PCE;
#endif
#ifdef GB_CORE_ENABLED
    if (ext == "gb" || ext == "gbc") return ROM_TYPE_GB;
#endif
#ifdef LYNX_CORE_ENABLED
    if (ext == "lnx") return ROM_TYPE_LYNX;
#endif
#ifdef SNES_CORE_ENABLED
    if (ext == "sfc") return ROM_TYPE_SNES;
    if (ext == "smc") return ROM_TYPE_SNES;
#endif
#ifdef MSX_CORE_ENABLED
    if (ext == "rom" || ext == "mx1") return ROM_TYPE_MSX;
#endif
#ifdef A7800_CORE_ENABLED
    if (ext == "a78") return ROM_TYPE_ATARI7800;
#endif
#ifdef A2600_CORE_ENABLED
    if (ext == "a26") return ROM_TYPE_ATARI2600;
#endif
#ifdef GX4000_CORE_ENABLED
    if (ext == "cpr") return ROM_TYPE_GX4000;
#endif

    return ROM_TYPE_UNKNOWN;
}

static inline std::string getRomPath(SdService& sdService, CardputerView& display, CardputerInput& input, const std::string& initialFolder = "/", bool skipWelcome = false) {
    VerticalSelector verticalSelector(display, input);
    std::vector<std::string> supportedExts = {
#ifdef NES_CORE_ENABLED
        ".nes",
#endif
#ifdef GB_CORE_ENABLED
        ".gb", ".gbc",
#endif
#ifdef SNES_CORE_ENABLED
        ".sfc", ".smc",
#endif
#ifdef SMS_CORE_ENABLED
        ".sms", ".gg", ".sg", ".sc", ".col",
#endif
#ifdef MD_CORE_ENABLED
        ".md",
#endif
#ifdef NGP_CORE_ENABLED
        ".ngp", ".ngc",
#endif
#ifdef WS_CORE_ENABLED
        ".ws", ".wsc",
#endif
#ifdef PCE_CORE_ENABLED
        ".pce",
#endif
#ifdef LYNX_CORE_ENABLED
        ".lnx",
#endif
#ifdef MSX_CORE_ENABLED
        ".rom", ".mx1",
#endif
#ifdef A2600_CORE_ENABLED
        ".a26",
#endif
#ifdef A7800_CORE_ENABLED
        ".a78",
#endif
#ifdef GX4000_CORE_ENABLED
        ".cpr",
#endif
    };

    display.initialize();

    if (!skipWelcome) {
        display.topBar("LOAD ROM CARTRIDGE", false, false);
        display.showValidExt(supportedExts);
        input.waitPress(3000);
    } else {
        display.topBar("LOAD ROM CARTRIDGE", false, false);
        display.subMessage("Loading...", 0);
    }

    if (!sdService.begin()) {
        display.subMessage("SD card not found", 2000);
        return "";
    }

    input.flushInput(1);
    std::string currentPath = initialFolder.empty() ? "/" : initialFolder;
    std::string previousPath;
    std::vector<std::string> elementNames;
    std::string savedCursorDir;
    std::string savedCursorItem;
    loadRomCursorFromNvs(savedCursorDir, savedCursorItem);
    int initialSelection = 0;

    while (true) {
        // List elements
        if (currentPath != previousPath) {
            display.subMessage("Loading...", 0);
            elementNames = sdService.getCachedDirectoryElements(currentPath);
            previousPath = currentPath;
            initialSelection = findRomCursorIndex(
                elementNames,
                currentPath,
                savedCursorDir,
                savedCursorItem);

            if (elementNames.empty()) {
                display.subMessage("No elements found", 2000);
                auto parent = sdService.getParentDirectory(currentPath);
                if (parent.empty() || parent == currentPath) {
                    sdService.close();
                    return "";
                }
                currentPath = parent;
                continue;
            }
        }

        // Select element
        int selectedIndex = verticalSelector.select(
            currentPath,
            elementNames,
            true,   // back item support
            true,   // UI extra
            {},
            {},
            false,
            false,
            initialSelection
        );
        initialSelection = 0;

        if (selectedIndex == VERTICAL_SELECTOR_G0) {
            showRomConfigMenu(sdService, display, input, currentPath, elementNames, previousPath);
            initialSelection = findRomCursorIndex(
                elementNames,
                currentPath,
                savedCursorDir,
                savedCursorItem);
            continue;
        }

        // Retour
        if (selectedIndex == VERTICAL_SELECTOR_BACK || selectedIndex >= (int)elementNames.size()) {
            if (currentPath == "/") {
                display.topBar("LOAD ROM CARTRIDGE", false, false);
                display.showValidExt(supportedExts);
                input.waitPress();
            } else {
                std::string parent = sdService.getParentDirectory(currentPath);
                std::string child = romSelectorBasename(currentPath);
                saveRomCursorToNvs(parent, child);
                savedCursorDir = parent;
                savedCursorItem = child;
                currentPath = parent;
            }
            continue;
        }

        // Construct next path
        std::string nextPath = currentPath;
        if (!nextPath.empty() && nextPath.back() != '/') {
            nextPath += "/";
        } 

        const std::string selectedName = elementNames[(size_t)selectedIndex];
        saveRomCursorToNvs(currentPath, selectedName);
        savedCursorDir = currentPath;
        savedCursorItem = selectedName;

        nextPath += selectedName;

        // folder
        if (sdService.isDirectory(nextPath)) {
            currentPath = nextPath;
            continue;
        // file
        } else {
            if (!hasRomExt(nextPath)) {
                display.topBar("SELECT A ROM FILE", false, false);
                display.showValidExt(supportedExts);
                input.waitPress();
                continue; // non rom file
            }
            return "/sd" + nextPath; // file selected
        }
    }

    return "";
}
