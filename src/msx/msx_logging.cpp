#include "msx_logging.h"

#include <Preferences.h>

#include <atomic>
#include <cstdarg>

namespace {

constexpr const char* kMsxLogConfigNs = "msx_log";
constexpr const char* kMsxLogEnabledKey = "enabled";
constexpr const char* kMsxLogMaskKey = "mask";

std::atomic<bool> s_msxLogsEnabled{false};
std::atomic<uint32_t> s_msxLogCategoryMask{0u};
std::atomic<bool> s_msxLogConfigLoaded{false};

void msx_logs_ensure_config_loaded(void)
{
    if (s_msxLogConfigLoaded.load(std::memory_order_acquire)) {
        return;
    }

    Preferences prefs;
    prefs.begin(kMsxLogConfigNs, true);
    const bool savedEnabled = prefs.getBool(kMsxLogEnabledKey, false);
    const uint32_t savedMask = prefs.getUInt(kMsxLogMaskKey, 0u);
    prefs.end();

    s_msxLogsEnabled.store(savedEnabled, std::memory_order_relaxed);
    s_msxLogCategoryMask.store(savedMask, std::memory_order_relaxed);
    s_msxLogConfigLoaded.store(true, std::memory_order_release);
}

void msx_logs_store_enabled(bool enabled)
{
    Preferences prefs;
    prefs.begin(kMsxLogConfigNs, false);
    prefs.putBool(kMsxLogEnabledKey, enabled);
    prefs.end();
}

void msx_logs_store_category_mask(uint32_t mask)
{
    Preferences prefs;
    prefs.begin(kMsxLogConfigNs, false);
    prefs.putUInt(kMsxLogMaskKey, mask);
    prefs.end();
}

} // namespace

bool msx_logs_enabled(void)
{
    msx_logs_ensure_config_loaded();
    return s_msxLogsEnabled.load(std::memory_order_relaxed);
}

void msx_logs_set_enabled(bool enabled)
{
    msx_logs_ensure_config_loaded();
    s_msxLogsEnabled.store(enabled, std::memory_order_relaxed);
    msx_logs_store_enabled(enabled);
}

bool msx_logs_toggle(void)
{
    const bool enabled = !msx_logs_enabled();
    msx_logs_set_enabled(enabled);
    return enabled;
}

int msx_log_printf(const char* format, ...)
{
    if (!format || !msx_logs_enabled()) {
        return 0;
    }

    va_list args;
    va_start(args, format);
    const int written = std::vprintf(format, args);
    va_end(args);
    return written;
}

uint32_t msx_log_category_mask(void)
{
    msx_logs_ensure_config_loaded();
    return s_msxLogCategoryMask.load(std::memory_order_relaxed);
}

bool msx_log_category_selected(MsxLogCategory category)
{
    const uint32_t mask = msx_log_category_mask();
    const uint32_t bit = static_cast<uint32_t>(category);
    return (mask & bit) != 0u;
}

bool msx_log_category_enabled(MsxLogCategory category)
{
    return msx_logs_enabled() && msx_log_category_selected(category);
}

void msx_log_category_set_enabled(MsxLogCategory category, bool enabled, bool persist)
{
    const uint32_t bit = static_cast<uint32_t>(category);
    uint32_t mask = msx_log_category_mask();
    mask = enabled ? (mask | bit) : (mask & ~bit);
    s_msxLogCategoryMask.store(mask, std::memory_order_relaxed);
    if (persist) {
        msx_logs_store_category_mask(mask);
    }
}

void msx_log_category_set_mask(uint32_t mask, bool persist)
{
    msx_logs_ensure_config_loaded();
    s_msxLogCategoryMask.store(mask, std::memory_order_relaxed);
    if (persist) {
        msx_logs_store_category_mask(mask);
    }
}

const char* msx_log_category_label(MsxLogCategory category)
{
    switch (category) {
        case MsxLogCategory::CoreTrace: return "CORE TRACE";
        case MsxLogCategory::Cart: return "CART";
        case MsxLogCategory::Scc: return "SCC";
        case MsxLogCategory::Profile: return "PROFILE";
        case MsxLogCategory::VdpCmd: return "VDP CMD";
        case MsxLogCategory::VdpFin: return "VDP FIN";
        case MsxLogCategory::VdpXfer: return "VDP XFER";
        case MsxLogCategory::VdpMode: return "VDP MODE";
        case MsxLogCategory::VdpG4Disp: return "VDP G4";
        case MsxLogCategory::VdpInit: return "VDP INIT";
        case MsxLogCategory::VdpDualcore: return "VDP DUAL";
        case MsxLogCategory::Bootstrap: return "BOOT";
        case MsxLogCategory::SccNotice: return "SCC NOTICE";
        case MsxLogCategory::SccAudio: return "SCC AUDIO";
        case MsxLogCategory::VdpSprite: return "VDP SPRITE";
        case MsxLogCategory::VdpCmdSeq: return "VDP CMDSEQ";
        case MsxLogCategory::VdpG4Addr: return "VDP G4 ADDR";
        case MsxLogCategory::VdpHighVram: return "VDP HIGH VRAM";
        case MsxLogCategory::VdpTrace: return "VDP TRACE";
        case MsxLogCategory::VdpBootDiag: return "VDP BOOT";
        case MsxLogCategory::PsgPeak: return "PEAK";
        default: return "LOG";
    }
}
