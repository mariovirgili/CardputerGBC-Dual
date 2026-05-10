#pragma once

#include <cstddef>
#include <cstdint>

class SdService;

void run_c64_prg(const uint8_t* prgData, size_t prgLen, const char* prgName, SdService& sd);
