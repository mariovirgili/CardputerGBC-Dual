#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cardputer/SdService.h"

void run_coleco(const uint8_t* romData, size_t romLen, const char* romName, SdService& sd);
