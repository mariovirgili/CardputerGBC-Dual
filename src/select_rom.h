#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "cardputer/SdService.h"
#include "cardputer/CardputerView.h"
#include "cardputer/VerticalSelector.h"
#include "cardputer/CardputerInput.h"
#include "last_game.h"

// Returns the absolute path of a supported ROM selected from the SD browser.
// - Navigates folders with the vertical selector
// - Returns the absolute path when a supported ROM is selected

enum RomType {
    ROM_TYPE_UNKNOWN = 0,
    ROM_TYPE_MSX,
    ROM_TYPE_MSX_DISK,
    ROM_TYPE_COLECO
};

static inline bool hasRomExt(const std::string& path) {
    if (path.size() < 3) return false;

    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) return false;

    std::string ext = path.substr(dotPos + 1);

    for (auto& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    return (ext == "rom" || ext == "dsk" || ext == "col");
}

static inline RomType getRomType(const std::string& path) {
    if (path.empty()) return ROM_TYPE_UNKNOWN;

    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos || dotPos + 1 >= path.size())
        return ROM_TYPE_UNKNOWN;

    std::string ext = path.substr(dotPos + 1);

    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == "rom") return ROM_TYPE_MSX;
    if (ext == "dsk") return ROM_TYPE_MSX_DISK;
    if (ext == "col") return ROM_TYPE_COLECO;

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
    std::vector<std::string> supportedExts = {".rom", ".dsk", ".col"};
    static constexpr size_t kRomBrowserMaxElements = 1024;
    static constexpr int kRomBrowserMenuResult = -3;
    auto releaseElementNames = [](std::vector<std::string>& names) {
        std::vector<std::string>().swap(names);
    };

    auto openRomBrowserMenu = [&](const std::string& folderPath) -> bool {
        VerticalSelector menuSelector(display, input);
        std::vector<std::string> menuOptions = {
            "Refresh current folder",
            "Cancel"
        };

        display.topBar("ROM SELECTOR MENU", true, false);
        const int menuSelection = menuSelector.select(
            normalizeRomFolderPath(folderPath),
            menuOptions,
            true,
            false,
            {},
            {},
            false,
            false,
            false,
            0
        );

        if (menuSelection == 0) {
            display.topBar("REFRESHING INDEX", true, false);
            display.subMessage("Scanning current folder", 0);
            (void)sdService.getCachedDirectoryElements(folderPath, &supportedExts, kRomBrowserMaxElements, true);
            display.subMessage("Folder index refreshed", 700);
            input.flushInput(100);
            return true;
        }

        input.flushInput(100);
        return false;
    };

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
        if (currentPath != previousPath) {
            display.subMessage("Loading...", 0);
            releaseElementNames(elementNames);
            std::vector<std::string> loadedElements =
                sdService.getCachedDirectoryElements(currentPath, &supportedExts, kRomBrowserMaxElements);
            elementNames.swap(loadedElements);
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

        const RomBrowserSelectionState browserSelection = getRomSelectionFromSd(sdService);
        const int preferredIndex = findPreferredRomIndex(
            elementNames,
            currentPath,
            lastRomPath,
            browserSelection
        );
        int selectedIndex = verticalSelector.select(
            currentPath,
            elementNames,
            true,
            true,
            {},
            {},
            false,
            false,
            true,
            preferredIndex,
            kRomBrowserMenuResult,
            -1
        );

        if (selectedIndex == kRomBrowserMenuResult) {
            if (openRomBrowserMenu(currentPath)) {
                releaseElementNames(elementNames);
                previousPath.clear();
            }
            continue;
        }

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

        std::string nextPath = currentPath;
        if (!nextPath.empty() && nextPath.back() != '/') {
            nextPath += "/";
        }

        nextPath += elementNames[selectedIndex];
        saveRomSelectionToSd(sdService, currentPath, elementNames[selectedIndex]);

        if (sdService.isDirectory(nextPath)) {
            currentPath = nextPath;
            continue;
        } else {
            if (!hasRomExt(nextPath)) {
                display.topBar("SELECT A ROM FILE", false, false);
                display.showValidExt(supportedExts);
                input.waitPress();
                continue;
            }
            saveRomFolderToSd(sdService, currentPath);
            return "/sd" + nextPath;
        }
    }

    return "";
}


