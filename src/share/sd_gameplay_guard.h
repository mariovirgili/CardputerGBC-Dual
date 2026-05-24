#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool share_sd_gameplay_close(const char* core_tag);
bool share_sd_gameplay_mount(const char* core_tag, const char* reason);
void share_sd_gameplay_close_if_mounted(const char* core_tag, const char* reason);

#ifdef __cplusplus
}
#endif
