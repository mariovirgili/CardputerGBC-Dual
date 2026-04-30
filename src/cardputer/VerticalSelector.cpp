#include "VerticalSelector.h"
#include <algorithm>
#include <cctype>

VerticalSelector::VerticalSelector(CardputerView& display, CardputerInput& input)
    : display(display), input(input) {}


int VerticalSelector::select(
        const std::string& title, 
        const std::vector<std::string>& options, 
        bool subMenu, 
        bool searchBar, 
        const std::vector<std::string>& options2,
        const std::vector<std::string>& shortcuts, 
        bool visibleMention,
        bool handleInactivity,
        bool romBrowserControls,
        int initialIndex,
        int longEscResult,
        int shortEscResult,
        VerticalSelectorChangedCallback changedCallback,
        void* changedContext)
{
    int currentIndex = 0, lastIndex = -1, lastQuerySize = 0;
    char key = KEY_NONE;
    std::string searchQuery;
    std::vector<std::string> filteredOptions;
    const std::vector<std::string>* activeOptions = &options;
    bool searchActive = false;
    bool lastSearchActive = false;

    auto optionCount = [&]() -> int {
        return static_cast<int>(activeOptions->size());
    };

    auto clampCurrentIndex = [&]() {
        if (activeOptions->empty()) {
            currentIndex = 0;
            return;
        }

        currentIndex = initialIndex;
        if (currentIndex < 0) {
            currentIndex = 0;
        }
        const int maxIndex = optionCount() - 1;
        if (currentIndex > maxIndex) {
            currentIndex = maxIndex;
        }
    };

    clampCurrentIndex();

    // Marquee state
    std::string mBase;
    size_t offset = 0;
    uint32_t lastMs = 0, pauseUntil = 0;

    // Scrolling
    const uint32_t STEP_MS = 200;
    const size_t VISIBLE_CHARS = 20;
    const int VISIBLE_ROWS = 4;
    const int PAGE_STEP = 4;
    const uint32_t PAGE_REPEAT_INITIAL_MS = 260;
    const uint32_t PAGE_REPEAT_STEP_MS = 80;
    char heldPageKey = KEY_NONE;
    uint32_t nextPageRepeatMs = 0;

    auto pageUp = [&]() {
        if (activeOptions->empty()) return;
        currentIndex -= PAGE_STEP;
        if (currentIndex < 0) currentIndex = 0;
    };

    auto pageDown = [&]() {
        if (activeOptions->empty()) return;
        currentIndex += PAGE_STEP;
        const int last = optionCount() - 1;
        if (currentIndex > last) currentIndex = last;
    };

    auto getHeldPageKey = [&]() -> char {
        if (!romBrowserControls) {
            return KEY_NONE;
        }

        if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_LEFT) ||
            M5Cardputer.Keyboard.isKeyPressed('a') ||
            M5Cardputer.Keyboard.isKeyPressed('A')) {
            return '\x11';
        }

        if (M5Cardputer.Keyboard.isKeyPressed(KEY_ARROW_RIGHT) ||
            M5Cardputer.Keyboard.isKeyPressed('d') ||
            M5Cardputer.Keyboard.isKeyPressed('D')) {
            return '\x12';
        }

        return KEY_NONE;
    };

    while (true) {
        const bool selectionChanged =
            (lastIndex != currentIndex) ||
            (lastQuerySize != (int)searchQuery.size()) ||
            (lastSearchActive != searchActive);

        // Full redraw
        if (selectionChanged) {
            display.topBar(searchActive ? searchQuery : title, subMenu, searchActive);
            display.verticalSelection(*activeOptions, currentIndex, VISIBLE_ROWS, options2, shortcuts, visibleMention);
            if (!activeOptions->empty()) mBase = (*activeOptions)[currentIndex];
            if (changedCallback && !activeOptions->empty()) {
                changedCallback(title, (*activeOptions)[currentIndex], changedContext);
            }
            offset = 0;
            lastMs = millis();
            lastIndex = currentIndex;
            lastQuerySize = (int)searchQuery.size();
            lastSearchActive = searchActive;
        } else if (!activeOptions->empty()) {
            // Redraw partial
            const std::string& base = (*activeOptions)[currentIndex];
            uint32_t now = millis();

            size_t startRow = (currentIndex / VISIBLE_ROWS) * VISIBLE_ROWS;
            uint16_t rowInPage = currentIndex - startRow;

            if (base.size() > VISIBLE_CHARS  && now - lastMs >= STEP_MS) {
                lastMs = now;

                if (offset >= base.size() - VISIBLE_CHARS) {
                    offset = 0;
                } else {
                    offset++;
                }

                std::string view = base.substr(offset);
                display.drawSelectedRowMarquee(view, rowInPage, VISIBLE_ROWS);
            }            
        }

        // INPUT
        key = input.handler();

        if (romBrowserControls && !searchActive) {
            const uint32_t now = millis();
            const char currentHeldPageKey = getHeldPageKey();

            if (currentHeldPageKey != heldPageKey) {
                heldPageKey = currentHeldPageKey;
                nextPageRepeatMs = (heldPageKey == KEY_NONE) ? 0 : (now + PAGE_REPEAT_INITIAL_MS);
            } else if (key == KEY_NONE &&
                       heldPageKey != KEY_NONE &&
                       now >= nextPageRepeatMs) {
                key = heldPageKey;
                nextPageRepeatMs = now + PAGE_REPEAT_STEP_MS;
            }
        }

        if (!shortcuts.empty() && !searchActive) {
            int si = checkShortcut(shortcuts, key);
            if (si != -1) return si;
        }

        if (searchBar && !searchActive && key == KEY_TAB_CUSTOM) {
            searchActive = true;
            searchQuery.clear();
            heldPageKey = KEY_NONE;
            nextPageRepeatMs = 0;
            continue;
        }

        if (romBrowserControls && !searchActive) {
            if (key == KEY_ARROW_LEFT) {
                key = '\x11';
            } else if (key == KEY_ARROW_RIGHT) {
                key = '\x12';
            } else {
                switch (std::tolower((unsigned char)key)) {
                    case 'e':
                        key = KEY_ARROW_UP;
                        break;
                    case 'z':
                        key = KEY_ARROW_DOWN;
                        break;
                    case 'p':
                        key = KEY_OK;
                        break;
                    case 'k':
                        key = KEY_ESC_CUSTOM;
                        break;
                    case KEY_GO_CUSTOM:
                        key = KEY_NONE;
                        break;
                    case 'a':
                        key = '\x11'; // page up sentinel
                        break;
                    case 'd':
                        key = '\x12'; // page down sentinel
                        break;
                    default:
                        break;
                }
            }
        }

        else if (!searchActive && !searchBar) {
            if (key == KEY_GO_CUSTOM) {
                key = KEY_ESC_CUSTOM;
            }

            switch (std::tolower((unsigned char)key)) {
                case 'e':
                    key = KEY_ARROW_UP;
                    break;
                case 'z':
                    key = KEY_ARROW_DOWN;
                    break;
                case 'a':
                    key = KEY_ARROW_LEFT;
                    break;
                case 'd':
                    key = KEY_ARROW_RIGHT;
                    break;
                case 'p':
                    key = KEY_OK;
                    break;
                default:
                    break;
            }
        }

        if (searchActive) {
            switch (key) {
                case KEY_OK:
                    if (searchQuery.empty()) {
                        filteredOptions.clear();
                        activeOptions = &options;
                    } else {
                        filteredOptions = filterOptions(options, searchQuery);
                        activeOptions = &filteredOptions;
                    }
                    currentIndex = 0;
                    searchActive = false;
                    break;
                case KEY_DEL:
                    if (!searchQuery.empty()) {
                        searchQuery.pop_back();
                    }
                    break;
                case KEY_ESC_CUSTOM:
                case KEY_ESC_LONG_CUSTOM:
                    searchQuery.clear();
                    searchActive = false;
                    break;
                default: {
                    const bool valid = std::isalnum((unsigned char)key) ||
                                       key == ' ' ||
                                       key == '-' ||
                                       key == '_' ||
                                       key == '.';
                    if (valid) {
                        searchQuery += key;
                    }
                    break;
                }
            }
            continue;
        }

        switch (key) {
            case KEY_ARROW_UP:
                if (!activeOptions->empty())
                    currentIndex = (currentIndex > 0) ? currentIndex - 1 : optionCount() - 1;
                break;
            case KEY_ARROW_DOWN:
                if (!activeOptions->empty())
                    currentIndex = (currentIndex < optionCount() - 1) ? currentIndex + 1 : 0;
                break;
            case '\x11':
                pageUp();
                break;
            case '\x12':
                pageDown();
                break;
            case KEY_ARROW_RIGHT:
            case KEY_OK:
                if (!activeOptions->empty()) {
                    if (activeOptions == &options) {
                        return currentIndex;
                    }
                    for (size_t i = 0; i < options.size(); ++i)
                        if (options[i] == (*activeOptions)[currentIndex]) return (int)i;
                }
                break;
            case KEY_ESC_CUSTOM:
                return shortEscResult;
            case KEY_ESC_LONG_CUSTOM:
                return longEscResult;
            case KEY_ARROW_LEFT:
                if (!romBrowserControls) {
                    return shortEscResult;
                }
                break;
            case KEY_DEL:
                break;
            default:
                break;
        }
    }
}

std::string VerticalSelector::toLowerCase(const std::string& input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::vector<std::string> VerticalSelector::filterOptions(const std::vector<std::string>& options, const std::string& query) {
    std::vector<std::string> filtered;
    std::string lowerQuery = toLowerCase(query);
    for (const auto& option : options) {
        if (toLowerCase(option).find(lowerQuery) != std::string::npos) {
            filtered.push_back(option);
        }
    }
    return filtered;
}

int VerticalSelector::checkShortcut(const std::vector<std::string>& shortcuts, char key) {
    for (size_t i = 0; i < shortcuts.size(); ++i) {
        if (!shortcuts[i].empty() && std::tolower(key) == std::tolower(shortcuts[i][0])) {
            return i;
        }
    }
    return -1;
}


