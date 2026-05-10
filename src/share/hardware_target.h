#pragma once

#include <M5Cardputer.h>

static inline m5::board_t cardputer_board_type()
{
    return M5.getBoard();
}

static inline bool cardputer_is_classic()
{
    return cardputer_board_type() == m5::board_t::board_M5Cardputer;
}

static inline bool cardputer_is_adv()
{
    return cardputer_board_type() == m5::board_t::board_M5CardputerADV;
}

static inline bool cardputer_has_external_tft()
{
    return cardputer_is_adv();
}
