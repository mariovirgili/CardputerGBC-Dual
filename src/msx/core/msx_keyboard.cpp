#include "msx_keyboard.h"

#include <cctype>
#include <cstring>

namespace {

inline void msx_keyboard_press(MsxKeyboardMatrix* matrix, uint8_t row, uint8_t bit)
{
    if (!matrix || row >= kMsxKeyboardRowCount || bit > 7u) {
        return;
    }

    matrix->rows[row] &= static_cast<uint8_t>(~(1u << bit));
}

char msx_keyboard_normalize_char(char ch)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

} // namespace

void msx_keyboard_matrix_clear(MsxKeyboardMatrix* matrix)
{
    if (!matrix) {
        return;
    }

    std::memset(matrix->rows, 0xFF, sizeof(matrix->rows));
}

bool msx_keyboard_matrix_press_ascii(MsxKeyboardMatrix* matrix, char ch)
{
    if (!matrix) {
        return false;
    }

    switch (msx_keyboard_normalize_char(ch)) {
        case '0':
        case ')':
            msx_keyboard_press(matrix, 0, 0);
            return true;
        case '1':
        case '!':
            msx_keyboard_press(matrix, 0, 1);
            return true;
        case '2':
        case '@':
            msx_keyboard_press(matrix, 0, 2);
            return true;
        case '3':
        case '#':
            msx_keyboard_press(matrix, 0, 3);
            return true;
        case '4':
        case '$':
            msx_keyboard_press(matrix, 0, 4);
            return true;
        case '5':
        case '%':
            msx_keyboard_press(matrix, 0, 5);
            return true;
        case '6':
        case '^':
            msx_keyboard_press(matrix, 0, 6);
            return true;
        case '7':
        case '&':
            msx_keyboard_press(matrix, 0, 7);
            return true;

        case '8':
        case '*':
            msx_keyboard_press(matrix, 1, 0);
            return true;
        case '9':
        case '(':
            msx_keyboard_press(matrix, 1, 1);
            return true;
        case '-':
        case '_':
            msx_keyboard_press(matrix, 1, 2);
            return true;
        case '=':
        case '+':
            msx_keyboard_press(matrix, 1, 3);
            return true;
        case '\\':
        case '|':
            msx_keyboard_press(matrix, 1, 4);
            return true;
        case '[':
        case '{':
            msx_keyboard_press(matrix, 1, 5);
            return true;
        case ']':
        case '}':
            msx_keyboard_press(matrix, 1, 6);
            return true;
        case ';':
        case ':':
            msx_keyboard_press(matrix, 1, 7);
            return true;

        case '\'':
        case '"':
            msx_keyboard_press(matrix, 2, 0);
            return true;
        case ',':
        case '<':
            msx_keyboard_press(matrix, 2, 2);
            return true;
        case '.':
        case '>':
            msx_keyboard_press(matrix, 2, 3);
            return true;
        case '/':
        case '?':
            msx_keyboard_press(matrix, 2, 4);
            return true;
        case 'a':
            msx_keyboard_press(matrix, 2, 6);
            return true;
        case 'b':
            msx_keyboard_press(matrix, 2, 7);
            return true;

        case 'c':
            msx_keyboard_press(matrix, 3, 0);
            return true;
        case 'd':
            msx_keyboard_press(matrix, 3, 1);
            return true;
        case 'e':
            msx_keyboard_press(matrix, 3, 2);
            return true;
        case 'f':
            msx_keyboard_press(matrix, 3, 3);
            return true;
        case 'g':
            msx_keyboard_press(matrix, 3, 4);
            return true;
        case 'h':
            msx_keyboard_press(matrix, 3, 5);
            return true;
        case 'i':
            msx_keyboard_press(matrix, 3, 6);
            return true;
        case 'j':
            msx_keyboard_press(matrix, 3, 7);
            return true;

        case 'k':
            msx_keyboard_press(matrix, 4, 0);
            return true;
        case 'l':
            msx_keyboard_press(matrix, 4, 1);
            return true;
        case 'm':
            msx_keyboard_press(matrix, 4, 2);
            return true;
        case 'n':
            msx_keyboard_press(matrix, 4, 3);
            return true;
        case 'o':
            msx_keyboard_press(matrix, 4, 4);
            return true;
        case 'p':
            msx_keyboard_press(matrix, 4, 5);
            return true;
        case 'q':
            msx_keyboard_press(matrix, 4, 6);
            return true;
        case 'r':
            msx_keyboard_press(matrix, 4, 7);
            return true;

        case 's':
            msx_keyboard_press(matrix, 5, 0);
            return true;
        case 't':
            msx_keyboard_press(matrix, 5, 1);
            return true;
        case 'u':
            msx_keyboard_press(matrix, 5, 2);
            return true;
        case 'v':
            msx_keyboard_press(matrix, 5, 3);
            return true;
        case 'w':
            msx_keyboard_press(matrix, 5, 4);
            return true;
        case 'x':
            msx_keyboard_press(matrix, 5, 5);
            return true;
        case 'y':
            msx_keyboard_press(matrix, 5, 6);
            return true;
        case 'z':
            msx_keyboard_press(matrix, 5, 7);
            return true;

        case ' ':
            msx_keyboard_press(matrix, 8, 0);
            return true;

        default:
            return false;
    }
}

void msx_keyboard_matrix_press_special(MsxKeyboardMatrix* matrix, MsxKeyboardSpecialKey key)
{
    switch (key) {
        case MsxKeyboardSpecialKey::Shift:
            msx_keyboard_press(matrix, 6, 0);
            break;
        case MsxKeyboardSpecialKey::Ctrl:
            msx_keyboard_press(matrix, 6, 1);
            break;
        case MsxKeyboardSpecialKey::Graph:
            msx_keyboard_press(matrix, 6, 2);
            break;
        case MsxKeyboardSpecialKey::Caps:
            msx_keyboard_press(matrix, 6, 3);
            break;
        case MsxKeyboardSpecialKey::Code:
            msx_keyboard_press(matrix, 6, 4);
            break;
        case MsxKeyboardSpecialKey::F1:
            msx_keyboard_press(matrix, 6, 5);
            break;
        case MsxKeyboardSpecialKey::F2:
            msx_keyboard_press(matrix, 6, 6);
            break;
        case MsxKeyboardSpecialKey::F3:
            msx_keyboard_press(matrix, 6, 7);
            break;
        case MsxKeyboardSpecialKey::F4:
            msx_keyboard_press(matrix, 7, 0);
            break;
        case MsxKeyboardSpecialKey::F5:
            msx_keyboard_press(matrix, 7, 1);
            break;
        case MsxKeyboardSpecialKey::Esc:
            msx_keyboard_press(matrix, 7, 2);
            break;
        case MsxKeyboardSpecialKey::Tab:
            msx_keyboard_press(matrix, 7, 3);
            break;
        case MsxKeyboardSpecialKey::Stop:
            msx_keyboard_press(matrix, 7, 4);
            break;
        case MsxKeyboardSpecialKey::Backspace:
            msx_keyboard_press(matrix, 7, 5);
            break;
        case MsxKeyboardSpecialKey::Select:
            msx_keyboard_press(matrix, 7, 6);
            break;
        case MsxKeyboardSpecialKey::Enter:
            msx_keyboard_press(matrix, 7, 7);
            break;
        case MsxKeyboardSpecialKey::Space:
            msx_keyboard_press(matrix, 8, 0);
            break;
        case MsxKeyboardSpecialKey::Home:
            msx_keyboard_press(matrix, 8, 1);
            break;
        case MsxKeyboardSpecialKey::Insert:
            msx_keyboard_press(matrix, 8, 2);
            break;
        case MsxKeyboardSpecialKey::Delete:
            msx_keyboard_press(matrix, 8, 3);
            break;
        case MsxKeyboardSpecialKey::Left:
            msx_keyboard_press(matrix, 8, 4);
            break;
        case MsxKeyboardSpecialKey::Up:
            msx_keyboard_press(matrix, 8, 5);
            break;
        case MsxKeyboardSpecialKey::Down:
            msx_keyboard_press(matrix, 8, 6);
            break;
        case MsxKeyboardSpecialKey::Right:
            msx_keyboard_press(matrix, 8, 7);
            break;
        default:
            break;
    }
}

void msx_keyboard_init(MsxKeyboardState* state)
{
    if (!state) {
        return;
    }

    msx_keyboard_matrix_clear(&state->matrix);
    state->selectedRow = 0;
}

void msx_keyboard_reset(MsxKeyboardState* state)
{
    msx_keyboard_init(state);
}

void msx_keyboard_set_matrix(MsxKeyboardState* state, const MsxKeyboardMatrix* matrix)
{
    if (!state || !matrix) {
        return;
    }

    state->matrix = *matrix;
}

void msx_keyboard_select_row(MsxKeyboardState* state, uint8_t row)
{
    if (!state) {
        return;
    }

    state->selectedRow = static_cast<uint8_t>(row & 0x0Fu);
}

uint8_t msx_keyboard_read_row(const MsxKeyboardState* state)
{
    if (!state || state->selectedRow >= kMsxKeyboardRowCount) {
        return 0xFFu;
    }

    return state->matrix.rows[state->selectedRow];
}
