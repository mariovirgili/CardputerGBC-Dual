#pragma once

#include <cstdio>

bool msx_logs_enabled(void);
void msx_logs_set_enabled(bool enabled);
bool msx_logs_toggle(void);
int msx_log_printf(const char* format, ...);

#define MSX_RUNTIME_LOG(...) do { (void)msx_log_printf(__VA_ARGS__); } while (0)
