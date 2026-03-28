#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "cardputer/SdService.h"
#include "cardputer/CardputerView.h"
#include "cardputer/VerticalSelector.h" 
#include "cardputer/CardputerInput.h"
#include "last_game.h"

// Returns the absolute path of a .nes selected file
// - Navigates folders with verticalSelector
// - When a selected item is a nes file, returns its path

enum RomType {
    ROM_TYPE_UNKNOWN = 0,
    ROM_TYPE_NES,
    ROM_TYPE_SMS,
    ROM_TYPE_GAMEGEAR,
    ROM_TYPE_NGP,
    ROM_TYPE_GENESIS,
    ROM_TYPE_WS,
    ROM_TYPE_PCE,
    ROM_TYPE_GB,
    ROM_TYPE_LYNX,
    ROM_TYPE_SNES,
    ROM_TYPE_A2600,
    ROM_TYPE_A7800
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

    return (ext == "nes" || ext == "gg" || ext == "sms" || ext == "ngc" || ext == "ngp" || ext == "md" || ext == "ws" || ext == "wsc" || ext == "pce" || ext == "gb" || ext == "gbc" || ext == "gb" || ext == "gbc" || ext == "lnx" || ext == "sfc" || ext == "smc" || ext == "a26" || ext == "a78");
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

    if (ext == "nes") return ROM_TYPE_NES;
    if (ext == "sms") return ROM_TYPE_SMS;
    if (ext == "gg")  return ROM_TYPE_GAMEGEAR;
    if (ext == "ngc") return ROM_TYPE_NGP;
    if (ext == "ngp") return ROM_TYPE_NGP;
    if (ext == "md")  return ROM_TYPE_GENESIS;
    if (ext == "ws" )  return ROM_TYPE_WS;
    if (ext == "wsc")  return ROM_TYPE_WS;
    if (ext == "pce")  return ROM_TYPE_PCE;
    if (ext == "gb" || ext == "gbc") return ROM_TYPE_GB;
    if (ext == "lnx") return ROM_TYPE_LYNX;
    if (ext == "sfc") return ROM_TYPE_SNES;
    if (ext == "smc") return ROM_TYPE_SNES;
    if (ext == "a26") return ROM_TYPE_A2600;
    if (ext == "a78") return ROM_TYPE_A7800;

    return ROM_TYPE_UNKNOWN;
}

static inline int findPreferredRomIndex(
    const std::vector<std::string>& elementNames,
    const std::string& currentPath,
    const std::string& lastRomPath,
    const RomBrowserSelectionState& browserSelection
) {
    if (elementNames.empty()) {
        return 0;
    }

    const std::string normalizedCurrentPath = normalizeRomFolderPath(currentPath);
    if (browserSelection.valid() &&
        browserSelection.folderPath == normalizedCurrentPath) {
        for (size_t i = 0; i < elementNames.size(); ++i) {
            if (elementNames[i] == browserSelection.entryName) {
                return static_cast<int>(i);
            }
        }
    }

    if (lastRomPath.empty()) {
        return 0;
    }

    const std::string normalizedLastRomPath = normalizeRomBrowserPath(lastRomPath);
    if (extractRomFolder(normalizedLastRomPath) != normalizedCurrentPath) {
        return 0;
    }

    const std::string preferredName = _basename(normalizedLastRomPath);
    for (size_t i = 0; i < elementNames.size(); ++i) {
        if (elementNames[i] == preferredName) {
            return static_cast<int>(i);
        }
    }

    return 0;
}

static inline std::string getRomPath(SdService& sdService, CardputerView& display, CardputerInput& input, const std::string& initialFolder = "/", bool skipWelcome = false) {
    VerticalSelector verticalSelector(display, input);
    std::vector<std::string> supportedExts = {".nes", ".gb", ".gbc", ".sfc", ".sms", ".md", ".gg", ".ngc", ".ws", ".wsc", ".pce", ".lnx", ".a26", ".a78"};

    display.initialize();
    display.topBar("LOAD ROM CARTRIDGE", false, false);

    if (!skipWelcome) {
        display.showValidExt(supportedExts);
        input.waitPress();
    } else {
        display.subMessage("Loading...", 0);
    }

    if (!sdService.begin()) {
        display.subMessage("SD card not found", 2000);
        return "";
    }

    input.flushInput(1);
    std::string currentPath = initialFolder.empty() ? "/" : initialFolder;
    std::string previousPath;
    const std::string lastRomPath = getLastGamePathFromNvs();
    std::vector<std::string> elementNames;

    while (true) {
        // List elements
        if (currentPath != previousPath) {
            display.subMessage("Loading...", 0);
            elementNames = sdService.getCachedDirectoryElements(currentPath);
            previousPath = currentPath;
            saveRomFolderToSd(sdService, currentPath);

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
        const RomBrowserSelectionState browserSelection = getRomSelectionFromSd(sdService);
        const int preferredIndex = findPreferredRomIndex(
            elementNames,
            currentPath,
            lastRomPath,
            browserSelection
        );
        uint16_t selectedIndex = verticalSelector.select(
            currentPath,
            elementNames,
            true,   // back item support
            true,   // UI extra
            {},
            {},
            false,
            false,
            true,
            preferredIndex
        );

        // Retour
        if (selectedIndex >= elementNames.size()) {
            if (currentPath == "/") {
                display.topBar("LOAD ROM CARTRIDGE", false, false);
                display.showValidExt(supportedExts);
                input.waitPress();
            } else {
                currentPath = sdService.getParentDirectory(currentPath);
            }
            continue;
        }

        // Construct next path
        std::string nextPath = currentPath;
        if (!nextPath.empty() && nextPath.back() != '/') {
            nextPath += "/";
        } 
            
        nextPath += elementNames[selectedIndex];
        saveRomSelectionToSd(sdService, currentPath, elementNames[selectedIndex]);

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
            saveRomFolderToSd(sdService, currentPath);
            return "/sd" + nextPath; // file selected
        }
    }

    return "";
}
