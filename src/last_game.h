#pragma once

#include <string>
#include <algorithm>
#include <cctype>
#include <Preferences.h>
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

static constexpr const char* ROM_BROWSER_STATE_DIR = "/.cardputer";
static constexpr const char* ROM_BROWSER_FOLDER_FILE = "/.cardputer/last_rom_folder.txt";

static inline std::string normalizeRomBrowserPath(const std::string& path) {
    if (path == "/sd") {
        return "/";
    }
    if (path.rfind("/sd/", 0) == 0) {
        return path.substr(3);
    }
    return path;
}

static inline std::string normalizeRomFolderPath(const std::string& folderPath) {
    std::string cleanPath = normalizeRomBrowserPath(folderPath);

    if (cleanPath.empty()) {
        return "/";
    }
    if (cleanPath.front() != '/') {
        cleanPath = "/" + cleanPath;
    }
    while (cleanPath.size() > 1 && cleanPath.back() == '/') {
        cleanPath.pop_back();
    }
    return cleanPath;
}

static inline std::string extractRomFolder(const std::string& filePath) {
    std::string cleanPath = normalizeRomFolderPath(filePath);
    size_t slash = cleanPath.find_last_of("/\\");
    if (slash == std::string::npos || slash == 0) {
        return "/";
    }
    return cleanPath.substr(0, slash);
}

static inline bool saveRomFolderToSd(
    SdService& sdService,
    const std::string& folderPath
) {
    if (!sdService.getSdState()) {
        return false;
    }
    if (!sdService.ensureDirectory(ROM_BROWSER_STATE_DIR)) {
        return false;
    }

    std::string cleanPath = normalizeRomFolderPath(folderPath);
    return sdService.writeFile(ROM_BROWSER_FOLDER_FILE, cleanPath + "\n");
}

static inline std::string getRomFolderFromSd(SdService& sdService) {
    if (!sdService.getSdState()) {
        return "";
    }

    std::string rawPath = sdService.readFile(ROM_BROWSER_FOLDER_FILE);
    if (rawPath.empty()) {
        return "";
    }

    while (!rawPath.empty() &&
           std::isspace(static_cast<unsigned char>(rawPath.back()))) {
        rawPath.pop_back();
    }

    size_t firstNonSpace = 0;
    while (firstNonSpace < rawPath.size() &&
           std::isspace(static_cast<unsigned char>(rawPath[firstNonSpace]))) {
        ++firstNonSpace;
    }

    if (firstNonSpace >= rawPath.size()) {
        return "";
    }

    std::string cleanPath = normalizeRomFolderPath(rawPath.substr(firstNonSpace));
    if (!sdService.isDirectory(cleanPath)) {
        return "";
    }

    return cleanPath;
}

static inline std::string getLastGameFromNvs(
    CardputerView& display,
    CardputerInput& input,
    SdService& sdService
) {
    Preferences prefs;
    prefs.begin("cardputer_emu", true);
    String lastGame = prefs.getString("last_game", "");
    prefs.end();

    // No last game saved
    if (lastGame.isEmpty()) {
        return "";
    }

    std::string path = lastGame.c_str();

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

static inline std::string getLastGamePathFromNvs() {
    Preferences prefs;
    prefs.begin("cardputer_emu", true);
    String lastGame = prefs.getString("last_game", "");
    prefs.end();

    if (lastGame.isEmpty()) {
        return "";
    }

    return normalizeRomBrowserPath(lastGame.c_str());
}

static inline void saveLastGameToNvs(const std::string& filePath) {
    if (filePath.empty()) return;

    Preferences prefs;
    prefs.begin("cardputer_emu", false);

    std::string cleanPath = normalizeRomBrowserPath(filePath);

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
    String lastGame = prefs.getString("last_game", "");
    prefs.end();

    if (lastGame.isEmpty()) {
        return "";
    }

    std::string path = lastGame.c_str();
    std::string folder = extractRomFolder(path);
    if (!sdService.isDirectory(folder)) {
        return "";
    }

    return folder;
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
