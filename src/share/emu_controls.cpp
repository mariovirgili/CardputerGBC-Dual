#include "emu_controls.h"

#include "../atari2600/a2600_display.h"
#include "../atari7800/a7800_config.h"
#include "../cardputer/CardputerInput.h"
#include "../cardputer/CardputerView.h"
#include "../cardputer/SdService.h"
#include "../cardputer/VerticalSelector.h"
#include "../msx/msx_config.h"
#include "input.h"

#include <M5Cardputer.h>

#include <array>
#include <cctype>
#include <cstring>
#include <utility>

namespace share {
namespace {

struct ControlEntry {
    EmuAction action;
    const char* id;
    const char* label;
    char defaultKey;
};

struct ProfileDef {
    const char* name;
    const char* fileName;
    const ControlEntry* entries;
    size_t entryCount;
};

constexpr ControlEntry kA2600Entries[] = {
    {EmuAction::Up, "up", "UP", 'e'},
    {EmuAction::Down, "down", "DOWN", 's'},
    {EmuAction::Left, "left", "LEFT", 'a'},
    {EmuAction::Right, "right", "RIGHT", 'd'},
    {EmuAction::A, "fire", "FIRE", 'l'},
    {EmuAction::Select, "select", "SELECT", '2'},
    {EmuAction::Start, "reset", "RESET", '1'},
};

constexpr ControlEntry kA7800Entries[] = {
    {EmuAction::Up, "up", "UP", 'e'},
    {EmuAction::Down, "down", "DOWN", 's'},
    {EmuAction::Left, "left", "LEFT", 'a'},
    {EmuAction::Right, "right", "RIGHT", 'd'},
    {EmuAction::A, "fire1", "FIRE 1", 'l'},
    {EmuAction::B, "fire2", "FIRE 2", 'k'},
    {EmuAction::Select, "select", "SELECT", '2'},
    {EmuAction::Start, "reset", "RESET", '1'},
    {EmuAction::Pause, "pause", "PAUSE", 'o'},
};

constexpr ControlEntry kMsxEntries[] = {
    {EmuAction::Up, "up", "UP", 'e'},
    {EmuAction::Down, "down", "DOWN", 's'},
    {EmuAction::Left, "left", "LEFT", 'a'},
    {EmuAction::Right, "right", "RIGHT", 'd'},
    {EmuAction::A, "fire1", "PRIMARY", 'l'},
    {EmuAction::B, "fire2", "SECONDARY", 'k'},
    {EmuAction::Start, "start", "START", '1'},
    {EmuAction::Select, "select", "MENU", '2'},
};

constexpr ProfileDef kProfiles[] = {
    {"A2600", "A2600.opt", kA2600Entries, sizeof(kA2600Entries) / sizeof(kA2600Entries[0])},
    {"A7800", "A7800.opt", kA7800Entries, sizeof(kA7800Entries) / sizeof(kA7800Entries[0])},
    {"MSX", "MSX.opt", kMsxEntries, sizeof(kMsxEntries) / sizeof(kMsxEntries[0])},
};

constexpr size_t kProfileCount = static_cast<size_t>(EmuProfile::Count);
constexpr size_t kActionCount = static_cast<size_t>(EmuAction::Count);

static std::array<std::array<char, kActionCount>, kProfileCount> s_bindings = {};
static bool s_initialized = false;

const ProfileDef& getProfileDef(EmuProfile profile) {
    return kProfiles[static_cast<size_t>(profile)];
}

char normalizeKey(char key) {
    if (key >= 'A' && key <= 'Z') {
        return static_cast<char>(key - 'A' + 'a');
    }
    return key;
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string profilePath(EmuProfile profile) {
    return std::string("/") + emuProfileFileName(profile);
}

const ControlEntry* findEntry(EmuProfile profile, EmuAction action) {
    const auto& def = getProfileDef(profile);
    for (size_t i = 0; i < def.entryCount; ++i) {
        if (def.entries[i].action == action) {
            return &def.entries[i];
        }
    }
    return nullptr;
}

const ControlEntry* findEntryById(EmuProfile profile, const std::string& id) {
    std::string resolvedId = id;
    if (profile == EmuProfile::MSX) {
        if (resolvedId == "ret") {
            resolvedId = "start";
        } else if (resolvedId == "space") {
            resolvedId = "select";
        }
    }

    const auto& def = getProfileDef(profile);
    for (size_t i = 0; i < def.entryCount; ++i) {
        if (resolvedId == def.entries[i].id) {
            return &def.entries[i];
        }
    }
    return nullptr;
}

void ensureInitialized() {
    if (s_initialized) {
        return;
    }

    for (size_t p = 0; p < kProfileCount; ++p) {
        s_bindings[p].fill(0);
        const auto& def = kProfiles[p];
        for (size_t i = 0; i < def.entryCount; ++i) {
            const auto actionIndex = static_cast<size_t>(def.entries[i].action);
            s_bindings[p][actionIndex] = normalizeKey(def.entries[i].defaultKey);
        }
    }

    s_initialized = true;
}

bool isReservedKey(char key) {
    switch (key) {
        case KEY_NONE:
        case KEY_OK:
        case KEY_DEL:
        case KEY_GO_CUSTOM:
        case KEY_ESC_CUSTOM:
        case KEY_ESC_LONG_CUSTOM:
        case CARDPUTER_SCREEN_TOGGLE:
        case CARDPUTER_VOL_UP_1:
        case CARDPUTER_VOL_DOWN_1:
        case CARDPUTER_BRIGHT_UP:
        case CARDPUTER_BRIGHT_DOWN:
            return true;
        default:
            return false;
    }
}

bool isEditableKey(char key) {
    key = normalizeKey(key);
    if (key < 32 || key > 126) {
        return false;
    }
    return !isReservedKey(key);
}

const ControlEntry* findEntryByKey(EmuProfile profile, char key) {
    key = normalizeKey(key);
    const auto& def = getProfileDef(profile);
    for (size_t i = 0; i < def.entryCount; ++i) {
        const auto actionIndex = static_cast<size_t>(def.entries[i].action);
        if (s_bindings[static_cast<size_t>(profile)][actionIndex] == key) {
            return &def.entries[i];
        }
    }
    return nullptr;
}

void assignKey(EmuProfile profile, EmuAction action, char key) {
    key = normalizeKey(key);
    auto& bindings = s_bindings[static_cast<size_t>(profile)];
    const size_t actionIndex = static_cast<size_t>(action);
    const char oldKey = bindings[actionIndex];

    if (oldKey == key) {
        return;
    }

    if (const ControlEntry* other = findEntryByKey(profile, key)) {
        const size_t otherIndex = static_cast<size_t>(other->action);
        std::swap(bindings[actionIndex], bindings[otherIndex]);
        return;
    }

    bindings[actionIndex] = key;
}

std::string keyLabel(char key) {
    key = normalizeKey(key);
    if (key == 0) {
        return "-";
    }

    if (key == ' ') {
        return "SPACE";
    }

    if (std::isalpha(static_cast<unsigned char>(key)) != 0) {
        char upper[2] = {static_cast<char>(std::toupper(static_cast<unsigned char>(key))), '\0'};
        return std::string(upper);
    }

    char buffer[2] = {key, '\0'};
    return std::string(buffer);
}

std::string capturePrompt(const char* label) {
    return std::string("SET ") + label;
}

} // namespace

const char* emuProfileName(EmuProfile profile) {
    ensureInitialized();
    return getProfileDef(profile).name;
}

const char* emuProfileFileName(EmuProfile profile) {
    ensureInitialized();
    return getProfileDef(profile).fileName;
}

void emuControlsResetDefaults(EmuProfile profile) {
    ensureInitialized();
    auto& bindings = s_bindings[static_cast<size_t>(profile)];
    bindings.fill(0);

    const auto& def = getProfileDef(profile);
    for (size_t i = 0; i < def.entryCount; ++i) {
        bindings[static_cast<size_t>(def.entries[i].action)] = normalizeKey(def.entries[i].defaultKey);
    }
}

bool emuControlsLoad(SdService& sd, EmuProfile profile) {
    ensureInitialized();
    emuControlsResetDefaults(profile);

    const std::string path = profilePath(profile);
    if (!sd.isFile(path)) {
        return false;
    }

    const std::string content = sd.readFile(path);
    if (content.empty()) {
        return false;
    }

    size_t start = 0;
    while (start < content.size()) {
        size_t end = content.find('\n', start);
        if (end == std::string::npos) {
            end = content.size();
        }

        std::string line = trim(content.substr(start, end - start));
        start = end + 1;

        if (line.empty() || line[0] == '#') {
            continue;
        }

        const size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            continue;
        }

        const std::string keyId = toLower(trim(line.substr(0, eqPos)));
        const std::string keyValue = trim(line.substr(eqPos + 1));
        if (keyValue.empty()) {
            continue;
        }

        const ControlEntry* entry = findEntryById(profile, keyId);
        if (!entry) {
            continue;
        }

        const char mappedKey = normalizeKey(keyValue[0]);
        if (!isEditableKey(mappedKey)) {
            continue;
        }

        assignKey(profile, entry->action, mappedKey);
    }

    return true;
}

bool emuControlsSave(SdService& sd, EmuProfile profile) {
    ensureInitialized();

    const auto& def = getProfileDef(profile);
    std::string content;
    content.reserve(def.entryCount * 12);

    for (size_t i = 0; i < def.entryCount; ++i) {
        const char key = emuControlKey(profile, def.entries[i].action);
        if (key == 0) {
            continue;
        }

        content += def.entries[i].id;
        content += '=';
        content += key;
        content += '\n';
    }

    return sd.writeFile(profilePath(profile), content);
}

char emuControlKey(EmuProfile profile, EmuAction action) {
    ensureInitialized();
    return s_bindings[static_cast<size_t>(profile)][static_cast<size_t>(action)];
}

bool emuControlPressed(EmuProfile profile, EmuAction action) {
    const char key = emuControlKey(profile, action);
    return key != 0 && M5Cardputer.Keyboard.isKeyPressed(key);
}

std::string emuControlKeyLabel(EmuProfile profile, EmuAction action) {
    return keyLabel(emuControlKey(profile, action));
}

std::vector<std::string> emuControlActionLabels(EmuProfile profile) {
    ensureInitialized();

    std::vector<std::string> labels;
    const auto& def = getProfileDef(profile);
    labels.reserve(def.entryCount);

    for (size_t i = 0; i < def.entryCount; ++i) {
        labels.emplace_back(def.entries[i].label);
    }

    return labels;
}

std::vector<std::string> emuControlKeyLabels(EmuProfile profile) {
    ensureInitialized();

    std::vector<std::string> labels;
    const auto& def = getProfileDef(profile);
    labels.reserve(def.entryCount);

    for (size_t i = 0; i < def.entryCount; ++i) {
        labels.push_back(emuControlKeyLabel(profile, def.entries[i].action));
    }

    return labels;
}

bool emuControlsEdit(SdService& sd, EmuProfile profile, CardputerView& display, CardputerInput& input) {
    ensureInitialized();

    const auto& def = getProfileDef(profile);
    const auto original = s_bindings[static_cast<size_t>(profile)];
    const bool hasInternalViewOption =
        profile == EmuProfile::A2600 ||
        profile == EmuProfile::A7800 ||
        profile == EmuProfile::MSX;
    const bool hasMachineOption = profile == EmuProfile::MSX;

    const int wideInternalView = 1;
    const int pixelInternalView = 0;

    auto loadInternalView = [&]() -> int {
        switch (profile) {
            case EmuProfile::A2600:
                return static_cast<int>(a2600_display_load_internal_view_mode());
            case EmuProfile::A7800:
                return static_cast<int>(a7800_config_load_internal_view_mode());
            case EmuProfile::MSX:
                return static_cast<int>(msx_config_load_internal_view_mode());
            default:
                return wideInternalView;
        }
    };

    auto applyInternalView = [&](int value, bool persist) {
        switch (profile) {
            case EmuProfile::A2600:
                a2600_display_set_internal_view_mode(
                    value == pixelInternalView
                        ? A2600InternalViewMode::PixelPerfect
                        : A2600InternalViewMode::Wide,
                    persist
                );
                break;
            case EmuProfile::A7800:
                a7800_config_set_internal_view_mode(
                    value == pixelInternalView
                        ? A7800InternalViewMode::PixelPerfect
                        : A7800InternalViewMode::Wide,
                    persist
                );
                break;
            case EmuProfile::MSX:
                msx_config_set_internal_view_mode(
                    value == pixelInternalView
                        ? MsxInternalViewMode::PixelPerfect
                        : MsxInternalViewMode::Wide,
                    persist
                );
                break;
            default:
                break;
        }
    };

    auto internalViewLabel = [&](int value) -> const char* {
        switch (profile) {
            case EmuProfile::A2600:
                return a2600_display_internal_view_mode_label(
                    value == pixelInternalView
                        ? A2600InternalViewMode::PixelPerfect
                        : A2600InternalViewMode::Wide
                );
            case EmuProfile::A7800:
                return a7800_config_internal_view_mode_label(
                    value == pixelInternalView
                        ? A7800InternalViewMode::PixelPerfect
                        : A7800InternalViewMode::Wide
                );
            case EmuProfile::MSX:
                return msx_config_internal_view_mode_label(
                    value == pixelInternalView
                        ? MsxInternalViewMode::PixelPerfect
                        : MsxInternalViewMode::Wide
                );
            default:
                return "WIDE";
        }
    };

    const int autoMachineMode = static_cast<int>(MsxMachineMode::Auto);
    int originalMachineMode = autoMachineMode;
    if (hasMachineOption) {
        originalMachineMode = static_cast<int>(msx_config_load_machine_mode());
    }
    int pendingMachineMode = originalMachineMode;

    auto machineModeLabel = [&](int value) -> const char* {
        return msx_config_machine_mode_label(static_cast<MsxMachineMode>(value));
    };

    auto applyMachineMode = [&](int value, bool persist) {
        if (hasMachineOption) {
            msx_config_set_machine_mode(static_cast<MsxMachineMode>(value), persist);
        }
    };

    int originalInternalView = wideInternalView;
    if (hasInternalViewOption) {
        originalInternalView = loadInternalView();
    }
    int pendingInternalView = originalInternalView;
    int selectedIndex = 0;

    VerticalSelector selector(display, input);

    while (true) {
        std::vector<std::string> values = emuControlKeyLabels(profile);
        std::vector<std::string> labels = emuControlActionLabels(profile);

        if (hasInternalViewOption) {
            labels.emplace_back("INT VIEW");
            values.emplace_back(internalViewLabel(pendingInternalView));
        }

        if (hasMachineOption) {
            labels.emplace_back("MACHINE");
            values.emplace_back(machineModeLabel(pendingMachineMode));
        }

        int nextIndex = static_cast<int>(def.entryCount);
        const int internalViewIndex = hasInternalViewOption ? nextIndex++ : -1;
        const int machineIndex = hasMachineOption ? nextIndex++ : -1;
        const int saveIndex = nextIndex++;
        const int defaultsIndex = nextIndex++;
        const int cancelIndex = nextIndex++;

        labels.emplace_back("SAVE");
        values.emplace_back("WRITE FILE");

        labels.emplace_back("DEFAULTS");
        values.emplace_back("RESTORE");

        labels.emplace_back("CANCEL");
        values.emplace_back("DISCARD");

        const int index = selector.select(std::string(emuProfileName(profile)) + " CONFIG",
                                          values,
                                          false,
                                          false,
                                          labels,
                                          {},
                                          false,
                                          true,
                                          false,
                                          selectedIndex);

        if (index < 0 || index == cancelIndex) {
            s_bindings[static_cast<size_t>(profile)] = original;
            if (hasInternalViewOption) {
                applyInternalView(originalInternalView, false);
            }
            if (hasMachineOption) {
                applyMachineMode(originalMachineMode, false);
            }
            return false;
        }

        selectedIndex = index;

        if (index == saveIndex) {
            if (emuControlsSave(sd, profile)) {
                if (hasInternalViewOption) {
                    applyInternalView(pendingInternalView, true);
                }
                if (hasMachineOption) {
                    applyMachineMode(pendingMachineMode, true);
                }
                display.subMessage(std::string(emuProfileFileName(profile)) + " saved", 700);
                return true;
            }

            display.subMessage("Save failed", 1200);
            continue;
        }

        if (index == defaultsIndex) {
            emuControlsResetDefaults(profile);
            if (hasInternalViewOption) {
                pendingInternalView = wideInternalView;
                applyInternalView(pendingInternalView, false);
            }
            if (hasMachineOption) {
                pendingMachineMode = autoMachineMode;
                applyMachineMode(pendingMachineMode, false);
            }
            display.subMessage("Defaults restored", 700);
            continue;
        }

        if (index == internalViewIndex) {
            VerticalSelector viewSelector(display, input);
            const std::vector<std::string> viewOptions = {"Wide", "Pixel perfect"};
            const int initialViewIndex =
                (pendingInternalView == wideInternalView) ? 0 : 1;

            const int selectedView = viewSelector.select("Internal view",
                                                         viewOptions,
                                                         false,
                                                         false,
                                                         {},
                                                         {},
                                                         false,
                                                         true,
                                                         true,
                                                         initialViewIndex);

            if (selectedView >= 0) {
                pendingInternalView =
                    (selectedView == 1) ? pixelInternalView : wideInternalView;
                applyInternalView(pendingInternalView, false);
                display.subMessage(std::string("Internal view -> ") +
                                   internalViewLabel(pendingInternalView), 700);
            }
            continue;
        }

        if (index == machineIndex) {
            VerticalSelector machineSelector(display, input);
            const std::vector<std::string> machineOptions = {"Auto", "MSX1", "MSX2"};
            int initialMachineIndex = 0;
            if (pendingMachineMode == static_cast<int>(MsxMachineMode::MSX1)) {
                initialMachineIndex = 1;
            } else if (pendingMachineMode == static_cast<int>(MsxMachineMode::MSX2)) {
                initialMachineIndex = 2;
            }

            const int selectedMachine = machineSelector.select("Machine mode",
                                                               machineOptions,
                                                               false,
                                                               false,
                                                               {},
                                                               {},
                                                               false,
                                                               true,
                                                               true,
                                                               initialMachineIndex);

            if (selectedMachine >= 0) {
                if (selectedMachine == 1) {
                    pendingMachineMode = static_cast<int>(MsxMachineMode::MSX1);
                } else if (selectedMachine == 2) {
                    pendingMachineMode = static_cast<int>(MsxMachineMode::MSX2);
                } else {
                    pendingMachineMode = autoMachineMode;
                }
                applyMachineMode(pendingMachineMode, false);
                display.subMessage(std::string("Machine -> ") + machineModeLabel(pendingMachineMode), 700);
            }
            continue;
        }

        const ControlEntry& entry = def.entries[index];
        display.topBar(capturePrompt(entry.label), false, false);
        display.subMessage("Press new key", 700);
        display.subMessage("GO cancels", 0);

        while (true) {
            const char key = input.handler();
            if (key == KEY_NONE) {
                delay(1);
                continue;
            }

            if (key == KEY_GO_CUSTOM || key == KEY_ESC_CUSTOM || key == KEY_ESC_LONG_CUSTOM) {
                break;
            }

            if (!isEditableKey(key)) {
                display.subMessage("Key not allowed", 900);
                continue;
            }

            assignKey(profile, entry.action, key);
            display.subMessage(std::string(entry.label) + " -> " + keyLabel(key), 700);
            break;
        }
    }
}

} // namespace share


