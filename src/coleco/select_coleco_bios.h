#pragma once

#include <string>
#include <vector>

#include "cardputer/SdService.h"
#include "cardputer/CardputerView.h"
#include "cardputer/CardputerInput.h"
#include "cardputer/VerticalSelector.h"

// Browse the SD card for a .rom file to use as ColecoVision BIOS.
// Returns the absolute filesystem path (e.g. /sd/bios/coleco.rom),
// or an empty string if the user cancelled.
// Paths inside SdService are SD-relative (e.g. "/bios"), the /sd prefix
// is added only on the returned value, matching the select_rom.h pattern.
static inline std::string selectColecoBiosPath(
    SdService& sdService,
    CardputerView& display,
    CardputerInput& input)
{
    VerticalSelector verticalSelector(display, input);
    std::vector<std::string> biosExts = {".rom"};
    static constexpr size_t kMaxElements = 256;

    // Start in /bios if it exists, otherwise SD root
    std::string currentPath = sdService.isDirectory("/bios") ? "/bios" : "/";
    std::string previousPath;
    std::vector<std::string> elementNames;

    display.initialize();
    display.topBar("SELECT COLECO BIOS", false, false);
    display.subMessage("Pick coleco.rom from SD", 1200);
    input.flushInput(1);

    while (true) {
        if (currentPath != previousPath) {
            display.subMessage("Loading...", 0);
            std::vector<std::string> loaded =
                sdService.getCachedDirectoryElements(currentPath, &biosExts, kMaxElements);
            elementNames.swap(loaded);
            previousPath = currentPath;

            if (elementNames.empty()) {
                display.subMessage("No .rom files here", 1500);
                std::string parent = sdService.getParentDirectory(currentPath);
                if (parent.empty() || parent == currentPath) {
                    return "";
                }
                currentPath = parent;
                continue;
            }
        }

        display.topBar("SELECT COLECO BIOS", false, false);
        int selected = verticalSelector.select(
            currentPath,
            elementNames,
            true,   // subMenu (enables back)
            false,  // searchBar
            {}, {}, false, false,
            true,   // romBrowserControls (folder navigation)
            0
        );

        // Back / ESC
        if (selected < 0 || selected >= (int)elementNames.size()) {
            std::string parent = sdService.getParentDirectory(currentPath);
            if (parent.empty() || parent == currentPath) {
                return ""; // cancelled at root
            }
            currentPath = parent;
            previousPath = "";
            continue;
        }

        // Build next path (SD-relative)
        std::string nextPath = currentPath;
        if (!nextPath.empty() && nextPath.back() != '/') {
            nextPath += "/";
        }
        nextPath += elementNames[selected];

        if (sdService.isDirectory(nextPath)) {
            currentPath = nextPath;
            previousPath = "";
            continue;
        }

        // File selected — return filesystem path with /sd prefix
        return "/sd" + nextPath;
    }
}
