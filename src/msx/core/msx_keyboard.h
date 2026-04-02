#pragma once

#include <stdint.h>

constexpr uint8_t kMsxKeyboardRowCount = 11;

struct MsxKeyboardMatrix {
    uint8_t rows[kMsxKeyboardRowCount];
};

struct MsxKeyboardState {
    MsxKeyboardMatrix matrix;
    uint8_t selectedRow;
};

enum class MsxKeyboardSpecialKey : uint8_t {
    Shift = 0,
    Ctrl,
    Graph,
    Caps,
    Code,
    F1,
    F2,
    F3,
    F4,
    F5,
    Esc,
    Tab,
    Stop,
    Backspace,
    Select,
    Enter,
    Space,
    Home,
    Insert,
    Delete,
    Left,
    Up,
    Down,
    Right,
};

void msx_keyboard_matrix_clear(MsxKeyboardMatrix* matrix);
bool msx_keyboard_matrix_press_ascii(MsxKeyboardMatrix* matrix, char ch);
void msx_keyboard_matrix_press_special(MsxKeyboardMatrix* matrix, MsxKeyboardSpecialKey key);

void msx_keyboard_init(MsxKeyboardState* state);
void msx_keyboard_reset(MsxKeyboardState* state);
void msx_keyboard_set_matrix(MsxKeyboardState* state, const MsxKeyboardMatrix* matrix);
void msx_keyboard_select_row(MsxKeyboardState* state, uint8_t row);
uint8_t msx_keyboard_read_row(const MsxKeyboardState* state);
