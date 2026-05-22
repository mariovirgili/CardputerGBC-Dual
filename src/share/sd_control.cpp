#include "sd_control.h"

#include "cardputer/SdService.h"
#include "compat/arduino_compat.h"
#include "share/emu_log_cpp.h"

namespace {
SdService* s_sd_service = nullptr;
}

namespace share {
void sdRegisterService(SdService* service)
{
    s_sd_service = service;
}
}

extern "C" bool share_sd_begin_retry(void)
{
    if (!s_sd_service) {
        EMU_LOG("[SD][CTRL] begin skipped: no service registered\n");
        return false;
    }

    if (s_sd_service->getSdState()) {
        EMU_LOG("[SD][CTRL] begin skipped: already mounted\n");
        return true;
    }

    for (int attempt = 1; attempt <= 3; ++attempt) {
        EMU_LOG("[SD][CTRL] begin attempt=%d\n", attempt);
        if (s_sd_service->begin()) {
            EMU_LOG("[SD][CTRL] begin ok\n");
            return true;
        }
        delay(100);
    }

    EMU_LOG("[SD][CTRL] begin failed after retries\n");
    return false;
}

extern "C" void share_sd_close(void)
{
    if (!s_sd_service) {
        EMU_LOG("[SD][CTRL] close skipped: no service registered\n");
        return;
    }

    if (!s_sd_service->getSdState()) {
        EMU_LOG("[SD][CTRL] close skipped: already unmounted\n");
        return;
    }

    EMU_LOG("[SD][CTRL] close start\n");
    s_sd_service->close();
    EMU_LOG("[SD][CTRL] close done\n");
}

extern "C" bool share_sd_is_mounted(void)
{
    return s_sd_service && s_sd_service->getSdState();
}
