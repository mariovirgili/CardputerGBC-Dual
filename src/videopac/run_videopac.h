#ifndef RUN_VIDEOPAC_H
#define RUN_VIDEOPAC_H

#include <cstdint>
#include <cstddef>
#include <string>
#include "cardputer/SdService.h"
#include "cardputer/CardputerInput.h"
#include "cardputer/CardputerView.h"

enum class VideopacBiosId : uint8_t {
    Odyssey2Ntsc = 0,
    VideopacPal,
    VideopacPlusG7400,
    VideopacPlusFrance,
};

struct VideopacBiosChoice {
    VideopacBiosId id;
    const char* label;
    const char* detail;
    const char* fileName;
    const char* expectedMd5;
};

struct VideopacBiosImage {
    VideopacBiosId id = VideopacBiosId::Odyssey2Ntsc;
    uint8_t* data = nullptr;
    size_t size = 0;
    char md5[33] = {};
};

const VideopacBiosChoice& videopac_bios_info(VideopacBiosId id);
std::string videopac_bios_sd_path(VideopacBiosId id);
std::string videopac_bios_vfs_path(VideopacBiosId id);
VideopacBiosId videopac_select_bios_for_rom(CardputerView& display,
                                             CardputerInput& input,
                                             const std::string& romPath);
bool videopac_load_bios_image(VideopacBiosId id,
                              VideopacBiosImage* outImage,
                              char* error,
                              size_t errorSize);
void videopac_free_bios_image(VideopacBiosImage* image);
int videopac_bios_to_pending_mode(VideopacBiosId id);
bool videopac_bios_from_pending_mode(int pendingMode, VideopacBiosId* outId);

// Entry point principale per l'emulatore Odyssey2 / Videopac
void run_videopac(const uint8_t* romData,
                  size_t romLen,
                  const char* romName,
                  SdService& sd,
                  const VideopacBiosImage& biosImage);

#endif // RUN_VIDEOPAC_H
