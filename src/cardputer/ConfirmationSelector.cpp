#include "ConfirmationSelector.h"
#include <cctype>

ConfirmationSelector::ConfirmationSelector(CardputerView& display, CardputerInput& input)
    : display(display), input(input) {}

bool ConfirmationSelector::select(const std::string& title, const std::string& description) {
    char key = KEY_NONE;
    display.topBar(title, false, false);
    display.confirmationPrompt(description);
    while (true) {
        key = input.handler();
        const char lowerKey = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));

        if (key == KEY_OK || key == KEY_ARROW_RIGHT || lowerKey == 'd') {
            return true;
        }
        if (key == KEY_ESC_CUSTOM || key == KEY_ESC_LONG_CUSTOM || key == KEY_ARROW_LEFT || lowerKey == 'a') {
            return false;
        }
        delay(5);
    }
}
