#pragma once

#include <cstdio>
#include <stdint.h>

enum class MsxLogCategory : uint32_t {
    CoreTrace = 1u << 0,
    Cart = 1u << 1,
    Scc = 1u << 2,
    Profile = 1u << 3,
    VdpCmd = 1u << 4,
    VdpFin = 1u << 5,
    VdpXfer = 1u << 6,
    VdpMode = 1u << 7,
    VdpG4Disp = 1u << 8,
    VdpInit = 1u << 9,
    VdpDualcore = 1u << 10,
    Bootstrap = 1u << 11,
    SccNotice = 1u << 12,
    SccAudio = 1u << 13,
    VdpSprite = 1u << 14,
    VdpCmdSeq = 1u << 15,
    VdpG4Addr = 1u << 16,
    VdpHighVram = 1u << 17,
    VdpTrace = 1u << 18,
    VdpBootDiag = 1u << 19,
    PsgPeak = 1u << 20,
    InputKbd = 1u << 21,
    InputI2c = 1u << 22,
};

bool msx_logs_enabled(void);
void msx_logs_set_enabled(bool enabled);
bool msx_logs_toggle(void);
int msx_log_printf(const char* format, ...);
uint32_t msx_log_category_mask(void);
bool msx_log_category_selected(MsxLogCategory category);
bool msx_log_category_enabled(MsxLogCategory category);
void msx_log_category_set_enabled(MsxLogCategory category, bool enabled, bool persist);
void msx_log_category_set_mask(uint32_t mask, bool persist);
const char* msx_log_category_label(MsxLogCategory category);

#define MSX_RUNTIME_LOG(...) \
    do { \
        if (msx_logs_enabled()) { \
            (void)msx_log_printf(__VA_ARGS__); \
        } \
    } while (0)

#define MSX_CATEGORY_LOG(category, ...) \
    do { \
        if (msx_log_category_enabled(category)) { \
            (void)msx_log_printf(__VA_ARGS__); \
        } \
    } while (0)
