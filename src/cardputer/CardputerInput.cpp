#include "CardputerInput.h"

char CardputerInput::handler() {
    static bool goLongHandled = false;
    static bool suppressGoClick = false;
    static constexpr uint32_t kGoLongPressMs = 700;
    static uint32_t suppressGoUntilMs = 0;

    // Update keyboard state
    M5Cardputer.update();

    // Bouton GO
    if (M5Cardputer.BtnA.isPressed()) {
        if (!goLongHandled && M5Cardputer.BtnA.pressedFor(kGoLongPressMs)) {
            goLongHandled = true;
            suppressGoClick = true;
            suppressGoUntilMs = millis() + 250;
            delay(20);
            return KEY_ESC_LONG_CUSTOM;
        }
    } else {
        if (goLongHandled) {
            goLongHandled = false;
            delay(10);
            return KEY_NONE;
        }
        if (suppressGoClick) {
            (void)M5Cardputer.BtnA.wasClicked();
            if (millis() >= suppressGoUntilMs) {
                suppressGoClick = false;
            }
            delay(10);
            return KEY_NONE;
        }
        if (M5Cardputer.BtnA.wasClicked()) {
            delay(20);
            return KEY_ESC_CUSTOM;
        }
    }
    
    if (M5Cardputer.Keyboard.isChange()) {

        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

            if (status.enter) { // go to next menu
                return KEY_OK;
            }
            if (status.del) { 
                return KEY_DEL;
            }
            
            if(M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT)) { // go back to previous menu
                return KEY_ARROW_LEFT;
            }

            if(M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_RIGHT)) { // go to next menu
                return KEY_ARROW_RIGHT;
            }

            for (auto c : status.word) {
                // Issue with %, the only key that requires 2 inputs to display
                if (c == '%') {
                    return '5'; 
                }
                return c; // return first char only
            }
        }
    }
    delay(10); // debounce
    return KEY_NONE;
}

void CardputerInput::waitPress(uint32_t timeoutMs) {
    uint32_t start = millis();

    for (;;) {
        char c = readChar();
        if (c != KEY_NONE) {
            return;
        }

        if (timeoutMs > 0 && (millis() - start) >= timeoutMs) {
            return;
        }

        delay(5);
    }
}

void CardputerInput::flushInput(size_t ms) {
    unsigned long start = millis();
    while(millis() - start < ms){
        M5Cardputer.update();
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
        status.reset();
        delay(1);
    }
}


char CardputerInput::readChar() {
    M5Cardputer.update();
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    
    // State change
    if (!M5Cardputer.Keyboard.isChange()) return KEY_NONE;
    if (!M5Cardputer.Keyboard.isPressed()) return KEY_NONE;

    for (auto c : status.word) {
        return c;
    }

    if (status.enter) return KEY_OK;
    if (status.del) return KEY_DEL;

    return KEY_NONE;
}
