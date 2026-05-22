#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
class SdService;

namespace share {
void sdRegisterService(SdService* service);
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool share_sd_begin_retry(void);
void share_sd_close(void);
bool share_sd_is_mounted(void);

#ifdef __cplusplus
}
#endif
