#include "msx_cart.h"

#include <cstring>

namespace {

uint8_t msx_normalize_bank(const MsxCartState* state, uint8_t bank)
{
    if (!state || state->bankCount8K == 0) {
        return 0;
    }
    return static_cast<uint8_t>(bank % state->bankCount8K);
}

void msx_set_ascii16_pair(MsxCartState* state, uint8_t pairIndex, uint8_t baseBank)
{
    if (!state) {
        return;
    }

    const uint8_t evenBank = static_cast<uint8_t>((baseBank & 0xFEu) % state->bankCount8K);
    const uint8_t oddBank = static_cast<uint8_t>((evenBank + 1u) % state->bankCount8K);

    if (pairIndex == 0) {
        state->windowBanks[0] = evenBank;
        state->windowBanks[1] = oddBank;
    } else {
        state->windowBanks[2] = evenBank;
        state->windowBanks[3] = oddBank;
    }
}

} // namespace

bool msx_cart_init(MsxCartState* state, const MsxRomImage* image)
{
    if (!state || !image || !image->data || image->size == 0 || image->bankCount8K == 0) {
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    state->rom = image->data;
    state->size = image->size;
    state->type = image->cartridgeType;
    state->bankCount8K = image->bankCount8K;
    state->entryPoint = image->entryPoint;
    state->initAddress = image->initAddress;
    state->directBootCandidate = image->hasAbHeader;
    state->bankSwitching =
        state->type == MsxCartridgeType::Ascii8 ||
        state->type == MsxCartridgeType::Ascii16 ||
        state->type == MsxCartridgeType::Konami ||
        state->type == MsxCartridgeType::KonamiScc;
    state->ready = true;

    msx_cart_reset(state);
    return true;
}

void msx_cart_reset(MsxCartState* state)
{
    if (!state || !state->ready) {
        return;
    }

    switch (state->type) {
        case MsxCartridgeType::Plain16K:
            state->windowBanks[0] = msx_normalize_bank(state, 0);
            state->windowBanks[1] = msx_normalize_bank(state, 1);
            state->windowBanks[2] = msx_normalize_bank(state, 0);
            state->windowBanks[3] = msx_normalize_bank(state, 1);
            break;
        case MsxCartridgeType::Plain32K:
            state->windowBanks[0] = msx_normalize_bank(state, 0);
            state->windowBanks[1] = msx_normalize_bank(state, 1);
            state->windowBanks[2] = msx_normalize_bank(state, 2);
            state->windowBanks[3] = msx_normalize_bank(state, 3);
            break;
        default:
            state->windowBanks[0] = msx_normalize_bank(state, 0);
            state->windowBanks[1] = msx_normalize_bank(state, 1);
            state->windowBanks[2] = msx_normalize_bank(state, 2);
            state->windowBanks[3] = msx_normalize_bank(state, 3);
            break;
    }
}

const uint8_t* msx_cart_window_ptr(const MsxCartState* state, uint8_t windowIndex)
{
    if (!state || !state->ready || !state->rom || state->size == 0) {
        return nullptr;
    }

    const uint8_t window = static_cast<uint8_t>(windowIndex & 0x03u);
    const uint8_t bank = msx_normalize_bank(state, state->windowBanks[window]);
    return state->rom + static_cast<size_t>(bank) * 0x2000u;
}

void msx_cart_write(MsxCartState* state, uint16_t address, uint8_t value)
{
    if (!state || !state->ready || !state->bankSwitching || state->bankCount8K == 0) {
        return;
    }

    switch (state->type) {
        case MsxCartridgeType::Ascii8:
            if (address >= 0x6000 && address < 0x8000) {
                const uint8_t reg = static_cast<uint8_t>((address - 0x6000u) >> 11);
                if (reg < 4) {
                    state->windowBanks[reg] = msx_normalize_bank(state, value);
                }
            }
            break;

        case MsxCartridgeType::Ascii16:
            if (address >= 0x6000 && address < 0x6800) {
                msx_set_ascii16_pair(state, 0, value);
            }
            else if (address >= 0x7000 && address < 0x7800) {
                msx_set_ascii16_pair(state, 1, value);
            }
            break;

        case MsxCartridgeType::Konami:
            if (address >= 0x6000 && address < 0x6800) {
                state->windowBanks[0] = msx_normalize_bank(state, value);
            }
            else if (address >= 0x8000 && address < 0x8800) {
                state->windowBanks[1] = msx_normalize_bank(state, value);
            }
            else if (address >= 0xA000 && address < 0xA800) {
                state->windowBanks[2] = msx_normalize_bank(state, value);
                state->windowBanks[3] = msx_normalize_bank(state, static_cast<uint8_t>(value + 1u));
            }
            break;

        case MsxCartridgeType::KonamiScc:
            if (address >= 0x5000 && address < 0x5800) {
                state->windowBanks[0] = msx_normalize_bank(state, value);
            }
            else if (address >= 0x7000 && address < 0x7800) {
                state->windowBanks[1] = msx_normalize_bank(state, value);
            }
            else if (address >= 0x9000 && address < 0x9800) {
                state->windowBanks[2] = msx_normalize_bank(state, value);
            }
            else if (address >= 0xB000 && address < 0xB800) {
                state->windowBanks[3] = msx_normalize_bank(state, value);
            }
            break;

        default:
            break;
    }
}
