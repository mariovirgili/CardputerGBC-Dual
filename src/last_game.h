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
static constexpr const char* ROM_BROWSER_SELECTION_FILE = "/.cardputer/last_rom_selection.txt";
static constexpr const char* PENDING_LAUNCH_ROM_KEY = "pending_rom";

struct RomBrowserSelectionState {
    std::string folderPath;
    std::string entryName;

    bool valid() const {
        return !folderPath.empty() && !entryName.empty();
    }
};

struct PendingLaunchState {
    std::string romPath;

    bool valid() const {
        return !romPath.empty();
    }
};

static inline std::string trimRomBrowserStateValue(const std::string& value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }

    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }

    return value.substr(first, last - first);
}

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

static inline bool saveRomSelectionToSd(
    SdService& sdService,
    const std::string& folderPath,
    const std::string& entryName
) {
    if (!sdService.getSdState() || entryName.empty()) {
        return false;
    }
    if (!sdService.ensureDirectory(ROM_BROWSER_STATE_DIR)) {
        return false;
    }

    std::string cleanPath = normalizeRomFolderPath(folderPath);
    return sdService.writeFile(
        ROM_BROWSER_SELECTION_FILE,
        cleanPath + "\n" + entryName + "\n"
    );
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

static inline RomBrowserSelectionState getRomSelectionFromSd(SdService& sdService) {
    RomBrowserSelectionState state;
    if (!sdService.getSdState()) {
        return state;
    }

    std::string raw = sdService.readFile(ROM_BROWSER_SELECTION_FILE);
    if (raw.empty()) {
        return state;
    }

    size_t newline = raw.find('\n');
    if (newline == std::string::npos) {
        return state;
    }

    std::string folder = trimRomBrowserStateValue(raw.substr(0, newline));
    std::string entry  = trimRomBrowserStateValue(raw.substr(newline + 1));

    if (folder.empty() || entry.empty()) {
        return state;
    }

    folder = normalizeRomFolderPath(folder);
    if (!sdService.isDirectory(folder)) {
        return state;
    }

    state.folderPath = folder;
    state.entryName = entry;
    return state;
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

static inline void clearLastGameFromNvs() {
    Preferences prefs;
    prefs.begin("cardputer_emu", false);
    prefs.remove("last_game");
    prefs.end();
}

static inline bool savePendingLaunchToNvs(const std::string& filePath) {
    if (filePath.empty()) {
        return false;
    }

    Preferences prefs;
    prefs.begin("cardputer_emu", false);

    std::string cleanPath = normalizeRomBrowserPath(filePath);
    const bool okPath = prefs.putString(PENDING_LAUNCH_ROM_KEY, cleanPath.c_str()) > 0;
    prefs.end();

    return okPath;
}

static inline void clearPendingLaunchFromNvs() {
    Preferences prefs;
    prefs.begin("cardputer_emu", false);
    prefs.remove(PENDING_LAUNCH_ROM_KEY);
    prefs.end();
}

static inline PendingLaunchState consumePendingLaunchFromNvs(SdService& sdService) {
    PendingLaunchState state;

    Preferences prefs;
    prefs.begin("cardputer_emu", false);
    String pendingRom = prefs.getString(PENDING_LAUNCH_ROM_KEY, "");
    prefs.remove(PENDING_LAUNCH_ROM_KEY);
    prefs.end();

    if (pendingRom.isEmpty()) {
        return state;
    }

    std::string cleanPath = normalizeRomBrowserPath(pendingRom.c_str());
    if (!sdService.isFile(cleanPath)) {
        return state;
    }

    state.romPath = "/sd" + cleanPath;
    return state;
}

static inline bool clearRomBrowserStateFromSd(SdService& sdService) {
    if (!sdService.getSdState()) {
        return false;
    }

    const bool folderCleared =
        !sdService.isFile(ROM_BROWSER_FOLDER_FILE) ||
        sdService.deleteFile(ROM_BROWSER_FOLDER_FILE);
    const bool selectionCleared =
        !sdService.isFile(ROM_BROWSER_SELECTION_FILE) ||
        sdService.deleteFile(ROM_BROWSER_SELECTION_FILE);

    return folderCleared && selectionCleared;
}

static inline void clearSavedRomState(SdService& sdService) {
    clearLastGameFromNvs();
    clearPendingLaunchFromNvs();
    (void)clearRomBrowserStateFromSd(sdService);
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
