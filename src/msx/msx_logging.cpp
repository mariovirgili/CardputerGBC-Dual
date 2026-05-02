#include "msx_logging.h"

#include <atomic>
#include <cstdarg>

namespace {

std::atomic<bool> s_msxLogsEnabled{true};

} // namespace

bool msx_logs_enabled(void)
{
    return s_msxLogsEnabled.load(std::memory_order_relaxed);
}

void msx_logs_set_enabled(bool enabled)
{
    s_msxLogsEnabled.store(enabled, std::memory_order_relaxed);
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
