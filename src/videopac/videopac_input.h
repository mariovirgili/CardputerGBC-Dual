#ifndef VIDEOPAC_INPUT_H
#define VIDEOPAC_INPUT_H

#include <cstdint>

struct VideopacInputState {
    bool up;
    bool down;
    bool left;
    bool right;
    bool action;
    bool quitRequested;
    bool menuVisible;
    bool menuChanged;
    bool videoModeToggleRequested;
    bool keyboardOnlyMode;
    bool inputModeChanged;
    bool keys[128]; // Tastiera a membrana
};

void videopac_input_init(void);
void videopac_input_poll(VideopacInputState* state);

#endif // VIDEOPAC_INPUT_H
