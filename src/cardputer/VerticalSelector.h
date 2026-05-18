#pragma GCC optimize ("Os")

#ifndef VERTICAL_SELECTOR_H
#define VERTICAL_SELECTOR_H

#include "compat/arduino_compat.h"
#include <string>
#include <vector>
#include <cardputer/CardputerView.h>
#include <cardputer/CardputerInput.h>

static constexpr int VERTICAL_SELECTOR_BACK = -1;
static constexpr int VERTICAL_SELECTOR_G0 = -2;

class VerticalSelector {
public:
    VerticalSelector(CardputerView& display, CardputerInput& input);
    int select(const std::string& title, const std::vector<std::string>& options, bool subMenu = false, bool searchBar = false,  const std::vector<std::string>& options2={},  const std::vector<std::string>& shortcuts={}, bool visibleMention=false, bool handleInactivity=true, int initialIndex=0);

private:
    CardputerView& display;
    CardputerInput& input;

    std::string toLowerCase(const std::string& input);
    std::vector<std::string> filterOptions(const std::vector<std::string>& options, const std::string& query);
    int checkShortcut(const std::vector<std::string>& shortcuts, char key);
};

#endif // VERTICAL_SELECTOR_H
