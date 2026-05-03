#include "msx_cart.h"

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

#include "../msx_logging.h"

#ifndef MSX_BOOTSTRAP_LOG_ENABLED
#define MSX_BOOTSTRAP_LOG_ENABLED 1
#endif

namespace {

constexpr size_t kMsxCartSramSize = 0x2000u;
constexpr uint8_t kMsxCartSramBank = 0xFFu;
constexpr uint16_t kMsxFmpacMagic = 0x694Du;

bool msx_cart_ensure_sram(MsxCartState* state);
bool msx_cart_sram_offset(const MsxCartState* state, uint16_t address, size_t* offset);

uint8_t msx_normalize_bank(const MsxCartState* state, uint8_t bank)
{
    if (!state || state->bankCount8K == 0) {
        return 0;
    }
    return static_cast<uint8_t>(bank % state->bankCount8K);
}

void msx_set_ascii16_pair(MsxCartState* state, uint8_t pairIndex, uint8_t page16K)
{
    if (!state) {
        return;
    }

    const uint8_t evenBank = static_cast<uint8_t>((static_cast<uint16_t>(page16K) << 1u) % state->bankCount8K);
    const uint8_t oddBank = static_cast<uint8_t>((evenBank + 1u) % state->bankCount8K);

    if (pairIndex == 0) {
        state->windowBanks[0] = evenBank;
        state->windowBanks[1] = oddBank;
    } else {
        state->windowBanks[2] = evenBank;
        state->windowBanks[3] = oddBank;
    }
}

void msx_set_ascii8_bank_or_sram(MsxCartState* state, uint8_t reg, uint8_t value)
{
    if (!state || reg >= 4u) {
        return;
    }

    if (value & state->bankCount8K) {
        if (msx_cart_ensure_sram(state)) {
            state->windowBanks[reg] = kMsxCartSramBank;
        }
    } else {
        state->windowBanks[reg] = msx_normalize_bank(state, value);
    }
}

bool msx_write_ascii8_sram_window(MsxCartState* state, uint16_t address, uint8_t value)
{
    size_t offset = 0u;
    if (!state || address < 0x8000u || address >= 0xC000u ||
        !msx_cart_sram_offset(state, address, &offset)) {
        return false;
    }

    state->sram[offset] = value;
    state->sramDirty = true;
    return true;
}

bool msx_cart_sram_offset(const MsxCartState* state, uint16_t address, size_t* offset)
{
    if (!state || !offset || !state->sram || state->sramSize == 0u ||
        address < 0x4000u || address >= 0xC000u) {
        return false;
    }

    if (state->type == MsxCartridgeType::Fmpac &&
        state->fmpacKey == kMsxFmpacMagic &&
        address >= 0x4000u && address < 0x5FFEu) {
        *offset = static_cast<size_t>(address & 0x1FFFu) % state->sramSize;
        return true;
    }

    const uint8_t window = static_cast<uint8_t>((address - 0x4000u) >> 13);
    if (window >= 4u || state->windowBanks[window] != kMsxCartSramBank) {
        return false;
    }

    if (state->type == MsxCartridgeType::Ascii16) {
        *offset = static_cast<size_t>(address & 0x07FFu) % state->sramSize;
    } else {
        *offset = static_cast<size_t>(address & 0x1FFFu) % state->sramSize;
    }
    return true;
}

bool msx_cart_uses_sram(MsxCartridgeType type)
{
    return type == MsxCartridgeType::Ascii8 ||
           type == MsxCartridgeType::Ascii16 ||
           type == MsxCartridgeType::Fmpac;
}

bool msx_cart_ensure_sram(MsxCartState* state)
{
    if (!state || !msx_cart_uses_sram(state->type)) {
        return true;
    }
    if (state->sram) {
        return true;
    }

    uint8_t* sram = static_cast<uint8_t*>(heap_caps_malloc(kMsxCartSramSize, MALLOC_CAP_8BIT));
    if (!sram) {
        sram = static_cast<uint8_t*>(heap_caps_malloc(kMsxCartSramSize, MALLOC_CAP_DEFAULT));
    }
    if (!sram) {
        std::printf("[MSX][CART] SRAM alloc failed size=%u\n", static_cast<unsigned>(kMsxCartSramSize));
        return false;
    }

    std::memset(sram, 0xFF, kMsxCartSramSize);
    if (state->type == MsxCartridgeType::Fmpac) {
        sram[0x1FFEu] = static_cast<uint8_t>(kMsxFmpacMagic & 0xFFu);
        sram[0x1FFFu] = static_cast<uint8_t>(kMsxFmpacMagic >> 8);
    }
    state->sram = sram;
    state->sramSize = kMsxCartSramSize;
    state->ownsSram = true;
    std::printf("[MSX][CART] SRAM ready size=%u\n", static_cast<unsigned>(kMsxCartSramSize));
    return true;
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
    state->headerOffset = image->headerOffset;
    state->type = image->cartridgeType;
    state->bankCount8K = image->bankCount8K;
    state->quirks = image->quirks;
    state->entryPoint = image->entryPoint;
    state->initAddress = image->initAddress;
    state->directBootCandidate = image->hasAbHeader;
    state->bankSwitching =
        state->type == MsxCartridgeType::Ascii8 ||
        state->type == MsxCartridgeType::Ascii16 ||
        state->type == MsxCartridgeType::Konami ||
        state->type == MsxCartridgeType::KonamiScc ||
        state->type == MsxCartridgeType::Fmpac;
    state->ready = true;

    msx_cart_reset(state);
#if MSX_BOOTSTRAP_LOG_ENABLED
    if (msx_log_category_enabled(MsxLogCategory::Bootstrap)) {
        MSX_RUNTIME_LOG("[MSX][BOOTDBG] cart reset banks=%u/%u/%u/%u bankSwitch=%u sram=%u\n",
                    static_cast<unsigned>(state->windowBanks[0]),
                    static_cast<unsigned>(state->windowBanks[1]),
                    static_cast<unsigned>(state->windowBanks[2]),
                    static_cast<unsigned>(state->windowBanks[3]),
                    state->bankSwitching ? 1u : 0u,
                    state->sram ? 1u : 0u);
    }
#endif
    return true;
}

void msx_cart_shutdown(MsxCartState* state)
{
    if (!state) {
        return;
    }
    if (state->sram && state->ownsSram) {
        heap_caps_free(state->sram);
    }
    std::memset(state, 0, sizeof(*state));
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
        case MsxCartridgeType::Plain64K:
            state->windowBanks[0] = msx_normalize_bank(state, 2);
            state->windowBanks[1] = msx_normalize_bank(state, 3);
            state->windowBanks[2] = msx_normalize_bank(state, 4);
            state->windowBanks[3] = msx_normalize_bank(state, 5);
            break;
        case MsxCartridgeType::Ascii16:
            msx_set_ascii16_pair(state, 0u, 0u);
            msx_set_ascii16_pair(state,
                                  1u,
                                  (state->quirks & MsxRomQuirkAscii16BootMirror) ? 0u : 1u);
            break;
        case MsxCartridgeType::Konami:
            // fMSX uses SetMegaROM(Slot, 0, 1, ROMMask, 1) — page 2 (8000-9FFF)
            // gets the last bank so games can boot from 8000h code.
            state->windowBanks[0] = 0u;
            state->windowBanks[1] = msx_normalize_bank(state, 1u);
            state->windowBanks[2] = msx_normalize_bank(state, state->bankCount8K > 0u ? static_cast<uint8_t>(state->bankCount8K - 1u) : 0u);
            state->windowBanks[3] = msx_normalize_bank(state, 1u);
            break;
        case MsxCartridgeType::KonamiScc:
            // fMSX uses SetMegaROM(Slot, 0, 1, 2, 3)
            state->windowBanks[0] = msx_normalize_bank(state, 0u);
            state->windowBanks[1] = msx_normalize_bank(state, 1u);
            state->windowBanks[2] = msx_normalize_bank(state, 2u);
            state->windowBanks[3] = msx_normalize_bank(state, 3u);
            break;
        case MsxCartridgeType::Fmpac:
            state->windowBanks[0] = msx_normalize_bank(state, 0u);
            state->windowBanks[1] = msx_normalize_bank(state, 1u);
            state->windowBanks[2] = msx_normalize_bank(state, 2u);
            state->windowBanks[3] = msx_normalize_bank(state, 3u);
            state->fmpacKey = 0u;
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
    if (state->type == MsxCartridgeType::Fmpac &&
        state->fmpacKey == kMsxFmpacMagic &&
        window == 0u) {
        if (!state->sram || state->sramSize < kMsxCartSramSize) {
            return nullptr;
        }
        return state->sram;
    }

    if (state->windowBanks[window] == kMsxCartSramBank) {
        if (!state->sram || state->sramSize < kMsxCartSramSize) {
            return nullptr;
        }
        return state->sram;
    }

    const uint8_t bank = msx_normalize_bank(state, state->windowBanks[window]);
    return state->rom + static_cast<size_t>(bank) * 0x2000u;
}

void msx_cart_ascii16_sram_write(MsxCartState* state, uint16_t address, uint8_t value)
{
    size_t offset = 0u;
    if (!state || !msx_cart_sram_offset(state, address, &offset)) {
        return;
    }

    for (size_t mirror = 0; mirror < kMsxCartSramSize; mirror += 0x0800u) {
        state->sram[mirror + offset] = value;
    }
    state->sramDirty = true;
}

void msx_cart_write(MsxCartState* state, uint16_t address, uint8_t value)
{
    if (!state || !state->ready || !state->bankSwitching || state->bankCount8K == 0) {
        return;
    }

    const uint8_t oldBanks[4] = {
        state->windowBanks[0],
        state->windowBanks[1],
        state->windowBanks[2],
        state->windowBanks[3]
    };

    switch (state->type) {
        case MsxCartridgeType::Ascii8:
            if (msx_write_ascii8_sram_window(state, address, value)) {
                return;
            }
            if (address >= 0x6000 && address < 0x8000) {
                const uint8_t reg = static_cast<uint8_t>((address - 0x6000u) >> 11);
                msx_set_ascii8_bank_or_sram(state, reg, value);
            }
            break;

        case MsxCartridgeType::Ascii16:
            if (address >= 0x8000 && address < 0xC000 && state->windowBanks[2] == kMsxCartSramBank) {
                msx_cart_ascii16_sram_write(state, address, value);
                return;
            }
            if (address >= 0x6000 && address < 0x8000) {
                const uint8_t sramSelectBit = state->bankCount8K;
                const bool registerWrite = (address & 0x0FFFu) == 0u;
                if (registerWrite || value <= sramSelectBit) {
                    const uint8_t pairIndex = (address & 0x1000u) ? 1u : 0u;
                    if ((value & sramSelectBit) != 0u) {
                        if (msx_cart_ensure_sram(state)) {
                            state->windowBanks[pairIndex * 2u] = kMsxCartSramBank;
                            state->windowBanks[pairIndex * 2u + 1u] = kMsxCartSramBank;
                        }
                    } else {
                        msx_set_ascii16_pair(state, pairIndex, value);
                    }
                }
            }
            break;

        case MsxCartridgeType::Konami:
            if (address >= 0x6000 && address < 0x6800) {
                state->windowBanks[1] = msx_normalize_bank(state, value);
            }
            else if (address >= 0x8000 && address < 0x8800) {
                state->windowBanks[2] = msx_normalize_bank(state, value);
            }
            else if (address >= 0xA000 && address < 0xA800) {
                state->windowBanks[3] = msx_normalize_bank(state, value);
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

        case MsxCartridgeType::Fmpac:
            if (msx_write_ascii8_sram_window(state, address, value)) {
                return;
            }
            if (address == 0x7FF7u) {
                const uint8_t evenBank = msx_normalize_bank(state, static_cast<uint8_t>(value << 1u));
                state->windowBanks[0] = evenBank;
                state->windowBanks[1] = msx_normalize_bank(state, static_cast<uint8_t>(evenBank + 1u));
            }
            else if (address == 0x7FF6u) {
                // OPL enable/disable latch on Panasonic FM-PAC cartridges.
            }
            else if (address == 0x5FFEu || address == 0x5FFFu) {
                if (msx_cart_ensure_sram(state)) {
                    if (address & 0x0001u) {
                        state->fmpacKey = static_cast<uint16_t>((state->fmpacKey & 0x00FFu) |
                                                                (static_cast<uint16_t>(value) << 8));
                    } else {
                        state->fmpacKey = static_cast<uint16_t>((state->fmpacKey & 0xFF00u) | value);
                    }
                }
            }
            else if (address >= 0x4000u &&
                     address < 0x5FFEu &&
                     state->fmpacKey == kMsxFmpacMagic &&
                     msx_cart_ensure_sram(state)) {
                size_t offset = 0u;
                if (msx_cart_sram_offset(state, address, &offset)) {
                    state->sram[offset] = value;
                }
                state->sramDirty = true;
                return;
            }
            else if (address >= 0x6000u && address < 0x8000u) {
                const uint8_t reg = static_cast<uint8_t>((address - 0x6000u) >> 11);
                msx_set_ascii8_bank_or_sram(state, reg, value);
            }
            break;

        default:
            break;
    }

    if ((oldBanks[0] != state->windowBanks[0]) ||
        (oldBanks[1] != state->windowBanks[1]) ||
        (oldBanks[2] != state->windowBanks[2]) ||
        (oldBanks[3] != state->windowBanks[3])) {
#if MSX_BOOTSTRAP_LOG_ENABLED
        static uint16_t s_bootCartBankLogCount = 0u;
        if (msx_log_category_enabled(MsxLogCategory::Bootstrap) &&
            s_bootCartBankLogCount < 96u) {
            MSX_RUNTIME_LOG("[MSX][BOOTDBG][CART] type=%u WR %04X <- %02X banks %u/%u/%u/%u -> %u/%u/%u/%u #%u\n",
                        static_cast<unsigned>(state->type),
                        static_cast<unsigned>(address),
                        static_cast<unsigned>(value),
                        static_cast<unsigned>(oldBanks[0]),
                        static_cast<unsigned>(oldBanks[1]),
                        static_cast<unsigned>(oldBanks[2]),
                        static_cast<unsigned>(oldBanks[3]),
                        static_cast<unsigned>(state->windowBanks[0]),
                        static_cast<unsigned>(state->windowBanks[1]),
                        static_cast<unsigned>(state->windowBanks[2]),
                        static_cast<unsigned>(state->windowBanks[3]),
                        static_cast<unsigned>(s_bootCartBankLogCount));
            ++s_bootCartBankLogCount;
        }
#endif
        static uint16_t s_cartBankLogCount = 0u;
        if (msx_log_category_enabled(MsxLogCategory::Cart) && s_cartBankLogCount < 128u) {
            MSX_CATEGORY_LOG(MsxLogCategory::Cart,
                        "[MSX][CART] %u WR %04X <- %02X banks %u/%u/%u/%u -> %u/%u/%u/%u #%u\n",
                        static_cast<unsigned>(state->type),
                        static_cast<unsigned>(address),
                        static_cast<unsigned>(value),
                        static_cast<unsigned>(oldBanks[0]),
                        static_cast<unsigned>(oldBanks[1]),
                        static_cast<unsigned>(oldBanks[2]),
                        static_cast<unsigned>(oldBanks[3]),
                        static_cast<unsigned>(state->windowBanks[0]),
                        static_cast<unsigned>(state->windowBanks[1]),
                        static_cast<unsigned>(state->windowBanks[2]),
                        static_cast<unsigned>(state->windowBanks[3]),
                        static_cast<unsigned>(s_cartBankLogCount));
            ++s_cartBankLogCount;
        }
    }
}

bool msx_cart_read_sram(const MsxCartState* state, uint16_t address, uint8_t* value)
{
    size_t offset = 0u;
    if (!value || !msx_cart_sram_offset(state, address, &offset)) {
        return false;
    }

    *value = state->sram[offset];
    return true;
}

bool msx_cart_prepare_sram(MsxCartState* state)
{
    return msx_cart_ensure_sram(state);
}

bool msx_cart_load_sram(MsxCartState* state, const uint8_t* data, size_t size)
{
    if (!state || !data || size == 0u || !msx_cart_uses_sram(state->type)) {
        return false;
    }

    if (!msx_cart_ensure_sram(state) || !state->sram || state->sramSize == 0u) {
        return false;
    }

    const size_t copySize = size < state->sramSize ? size : state->sramSize;
    std::memcpy(state->sram, data, copySize);
    if (copySize < state->sramSize) {
        std::memset(state->sram + copySize, 0xFF, state->sramSize - copySize);
    }
    if (state->type == MsxCartridgeType::Fmpac && state->sramSize >= 0x2000u) {
        state->sram[0x1FFEu] = static_cast<uint8_t>(kMsxFmpacMagic & 0xFFu);
        state->sram[0x1FFFu] = static_cast<uint8_t>(kMsxFmpacMagic >> 8);
    }
    state->sramDirty = false;
    return true;
}

const uint8_t* msx_cart_sram_data(const MsxCartState* state)
{
    return state ? state->sram : nullptr;
}

size_t msx_cart_sram_size(const MsxCartState* state)
{
    return (state && state->sram) ? state->sramSize : 0u;
}

bool msx_cart_sram_dirty(const MsxCartState* state)
{
    return state && state->sramDirty;
}

void msx_cart_clear_sram_dirty(MsxCartState* state)
{
    if (state) {
        state->sramDirty = false;
    }
}
