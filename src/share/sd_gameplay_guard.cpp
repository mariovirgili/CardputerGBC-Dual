#include "share/sd_gameplay_guard.h"

#include "share/emu_log_cpp.h"
#include "share/sd_control.h"

static const char* sd_guard_tag(const char* core_tag)
{
    return (core_tag && core_tag[0]) ? core_tag : "EMU";
}

extern "C" bool share_sd_gameplay_close(const char* core_tag)
{
    const char* tag = sd_guard_tag(core_tag);
    if (!share_sd_is_mounted()) {
        EMU_LOG("[%s][SD] already off for gameplay\n", tag);
        return false;
    }

    EMU_LOG("[%s][SD] close for gameplay\n", tag);
    share_sd_close();
    return true;
}

extern "C" bool share_sd_gameplay_mount(const char* core_tag, const char* reason)
{
    const char* tag = sd_guard_tag(core_tag);
    const char* why = (reason && reason[0]) ? reason : "storage";

    if (share_sd_is_mounted()) {
        EMU_LOG("[%s][SD] already mounted for %s\n", tag, why);
        return true;
    }

    EMU_LOG("[%s][SD] remount for %s\n", tag, why);
    return share_sd_begin_retry();
}

extern "C" void share_sd_gameplay_close_if_mounted(const char* core_tag, const char* reason)
{
    const char* tag = sd_guard_tag(core_tag);
    const char* why = (reason && reason[0]) ? reason : "storage";

    if (!share_sd_is_mounted()) {
        EMU_LOG("[%s][SD] already off after %s\n", tag, why);
        return;
    }

    EMU_LOG("[%s][SD] close after %s\n", tag, why);
    share_sd_close();
}
