#pragma once

#include <stdint.h>
#include <string>
#include <vector>

class SdService;
class CardputerView;
class CardputerInput;

namespace share {

enum class EmuProfile : uint8_t {
    Nes = 0,
    Sms,
    Ngp,
    Ws,
    Pce,
    Gbc,
    Lynx,
    Genesis,
    Snes,
    Count
};

enum class EmuAction : uint8_t {
    Up = 0,
    Down,
    Left,
    Right,
    A,
    B,
    C,
    X,
    Y,
    L,
    R,
    Start,
    Select,
    Option,
    Option2,
    Pause,
    X1,
    X2,
    X3,
    X4,
    Y1,
    Y2,
    Y3,
    Y4,
    Count
};

const char* emuProfileName(EmuProfile profile);
const char* emuProfileFileName(EmuProfile profile);

void emuControlsResetDefaults(EmuProfile profile);
bool emuControlsLoad(SdService& sd, EmuProfile profile);
bool emuControlsSave(SdService& sd, EmuProfile profile);

char emuControlKey(EmuProfile profile, EmuAction action);
bool emuControlPressed(EmuProfile profile, EmuAction action);
std::string emuControlKeyLabel(EmuProfile profile, EmuAction action);
std::vector<std::string> emuControlActionLabels(EmuProfile profile);
std::vector<std::string> emuControlKeyLabels(EmuProfile profile);

bool emuControlsEdit(SdService& sd, EmuProfile profile, CardputerView& display, CardputerInput& input);

} // namespace share
