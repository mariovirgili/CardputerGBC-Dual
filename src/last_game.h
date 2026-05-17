#pragma GCC optimize ("Os")

#pragma once

#include <string>
#include <algorithm>
#include "compat/preferences_compat.h"
#include "cardputer/SdService.h"
#include "cardputer/CardputerView.h"
#include "cardputer/CardputerInput.h"
#include "cardputer/ConfirmationSelector.h"

// Helper: basename from path
static inline std::string _basename(const std::string& path) {
    if (path.empty()) return "";
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

static inline std::string getLastGameFromNvs(
    CardputerView& display,
    CardputerInput& input,
    SdService& sdService
) {
    Preferences prefs;
    prefs.begin("cardputer_emu", true);
    std::string lastGame = prefs.getString("last_game", "");
    prefs.end();

    // No last game saved
    if (lastGame.empty()) {
        return "";
    }

    std::string path = lastGame;

    display.topBar("GETTING LAST GAME", false, false);
    display.subMessage("Loading...", 0);

    // Verify if the file still exists
    if (!sdService.isFile(path)) {
        return "";
    }

    // User confirmation
    ConfirmationSelector confirm(display, input);
    bool confirmed = confirm.select("RESUME LAST GAME?", _basename(path));

    return confirmed ? path : "";
}

static inline void saveLastGameToNvs(const std::string& filePath) {
    if (filePath.empty()) return;

    Preferences prefs;
    prefs.begin("cardputer_emu", false);

    std::string cleanPath = filePath;
    if (cleanPath.rfind("/sd/", 0) == 0)
        cleanPath = cleanPath.substr(3); // retire /sd

    prefs.putString("last_game", cleanPath.c_str());
    prefs.end();
}

static inline std::string getRomFolderFromNvs(
    CardputerView& display,
    CardputerInput& input,
    SdService& sdService
) {
    Preferences prefs;
    prefs.begin("cardputer_emu", true);
    std::string lastGame = prefs.getString("last_game", "");
    prefs.end();

    if (lastGame.empty()) {
        return "";
    }

    std::string path = lastGame;

    // Get the folder
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return "";
    }

    return path.substr(0, slash);
}

static inline bool isQuittingGame() {
    Preferences prefs;
    prefs.begin("cardputer_emu", false);
    bool quitting = prefs.getBool("quit_game", false);

    if (quitting) {
        prefs.putBool("quit_game", false);
    }

    prefs.end();
    return quitting;
}
