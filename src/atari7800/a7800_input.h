#pragma once

#include <stdbool.h>
#include <stdint.h>

struct A7800InputState {
    bool up;
    bool down;
    bool left;
    bool right;
    bool fire1;
    bool fire2;
    bool select;
    bool reset;
    bool pause;
    bool quitRequested;
};

void a7800_input_init(void);
void a7800_input_poll(A7800InputState* state);
