#include "msx_media.h"

#include <SD.h>
#include <esp_heap_caps.h>
#include <mbedtls/md5.h>

#include <cstdio>
#include <cstring>

#include "../share/emu_static_pool.h"

extern const uint8_t cbios_main_msx1_rom_start[] asm("_binary_bios_cbios_0_29a_roms_cbios_main_msx1_rom_start");
extern const uint8_t cbios_main_msx1_rom_end[] asm("_binary_bios_cbios_0_29a_roms_cbios_main_msx1_rom_end");
extern const uint8_t cbios_sub_rom_start[] asm("_binary_bios_cbios_0_29a_roms_cbios_sub_rom_start");
extern const uint8_t cbios_sub_rom_end[] asm("_binary_bios_cbios_0_29a_roms_cbios_sub_rom_end");

#ifndef MSX_BIOS_LOG_ENABLED
#define MSX_BIOS_LOG_ENABLED 1
#endif

#if MSX_BIOS_LOG_ENABLED
#define MSX_BIOS_LOG(...) std::printf(__VA_ARGS__)
#else
#define MSX_BIOS_LOG(...) do { } while (0)
#endif

namespace {

constexpr size_t kMsxPageSize8K = 0x2000;
constexpr size_t kMsxHeaderStride = 0x2000;
constexpr size_t kMsxMaxHeaderProbe = 0x10000;
constexpr size_t kMsxMainBiosMinSize = 0x4000;
constexpr size_t kMsxMainBiosMaxSize = 0x10000;
constexpr size_t kMsxSubRomExactSize = 0x4000;
constexpr size_t kMsxMainBiosStaticSize = 0x8000;
constexpr const char* kMsx1BiosName = "MSX.ROM";
constexpr const char* kMsx1BiosMd5 = "364a1a579fe5cb8dba54519bcfcdac0d";
constexpr const char* kMsx2BiosName = "MSX2.ROM";
constexpr const char* kMsx2BiosMd5 = nullptr;
constexpr const char* kMsx2ExtBiosName = "MSX2EXT.ROM";
constexpr const char* kMsx2ExtBiosMd5 = nullptr;
constexpr const char* kMsxEmbeddedCbiosName = "C-BIOS MSX1";
constexpr const char* kMsxEmbeddedCbiosPath = "[embedded]/cbios_main_msx1.rom";
constexpr const char* kMsxEmbeddedCbiosSubName = "C-BIOS MSX2 SUB";
constexpr const char* kMsxEmbeddedCbiosSubPath = "[embedded]/cbios_sub.rom";
constexpr size_t kMsxMainBiosStaticOffset = EMU_STATIC_POOL_SIZE - kMsxMainBiosStaticSize;

static_assert(EMU_STATIC_POOL_SIZE >= (kMsxMainBiosStaticSize + kMsxPageSize8K),
              "MSX static pool too small for official BIOS fallback");

static bool s_msxMainBiosStaticUsed = false;

uint8_t* msx_main_bios_static_buffer_if_acquired()
{
    uint8_t* pool = emu_static_pool_get();
    return pool ? (pool + kMsxMainBiosStaticOffset) : nullptr;
}

uint8_t* msx_main_bios_static_buffer()
{
    if (!emu_static_pool_acquire()) {
        return nullptr;
    }

    return msx_main_bios_static_buffer_if_acquired();
}

bool msx_is_cart_exec_address(uint16_t address)
{
    return (address >= 0x4000u) && (address < 0xC000u);
}

bool msx_is_cart_text_address(uint16_t address)
{
    return (address >= 0x8000u) && (address < 0xC000u);
}

bool msx_has_ab_signature_at(const uint8_t* data, size_t size, size_t offset)
{
    return data && (offset + 2u <= size) && data[offset] == 'A' && data[offset + 1u] == 'B';
}

bool msx_is_plausible_header_at(const uint8_t* data, size_t size, size_t offset)
{
    if (!msx_has_ab_signature_at(data, size, offset) || (offset + 10u > size)) {
        return false;
    }

    const uint16_t init = static_cast<uint16_t>(data[offset + 2u] | (static_cast<uint16_t>(data[offset + 3u]) << 8));
    const uint16_t statement = static_cast<uint16_t>(data[offset + 4u] | (static_cast<uint16_t>(data[offset + 5u]) << 8));
    const uint16_t device = static_cast<uint16_t>(data[offset + 6u] | (static_cast<uint16_t>(data[offset + 7u]) << 8));
    const uint16_t text = static_cast<uint16_t>(data[offset + 8u] | (static_cast<uint16_t>(data[offset + 9u]) << 8));

    return msx_is_cart_exec_address(init) ||
           msx_is_cart_exec_address(statement) ||
           msx_is_cart_exec_address(device) ||
           msx_is_cart_text_address(text);
}

struct CandidateEntry {
    const char* path;
    const char* expectedName;
    const char* expectedMd5;
};

struct CandidateList {
    CandidateEntry entries[8];
    size_t count;
};

const char* msx_sd_open_path(const char* path)
{
    if (!path) {
        return "";
    }

    if (std::strncmp(path, "/sd/", 4) == 0) {
        return path + 3;
    }
    if (std::strcmp(path, "/sd") == 0) {
        return "/";
    }
    return path;
}

void msx_copy_string(char* dst, size_t dstSize, const char* src)
{
    if (!dst || dstSize == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    std::snprintf(dst, dstSize, "%s", src);
}

void msx_set_message(MsxBiosBundle* bundle, const char* message)
{
    if (!bundle) {
        return;
    }

    msx_copy_string(bundle->message, sizeof(bundle->message), message ? message : "");
}

void msx_append_candidate(CandidateList* list, const char* path, const char* expectedName, const char* expectedMd5)
{
    if (!list || !path || path[0] == '\0') {
        return;
    }

    for (size_t i = 0; i < list->count; ++i) {
        if (std::strcmp(list->entries[i].path, path) == 0) {
            return;
        }
    }

    if (list->count < (sizeof(list->entries) / sizeof(list->entries[0]))) {
        CandidateEntry& entry = list->entries[list->count++];
        entry.path = path;
        entry.expectedName = expectedName;
        entry.expectedMd5 = expectedMd5;
    }
}

bool msx_is_valid_main_bios_size(size_t size)
{
    if (size < kMsxMainBiosMinSize || size > kMsxMainBiosMaxSize) {
        return false;
    }

    return (size % kMsxPageSize8K) == 0;
}

bool msx_is_valid_subrom_size(size_t size)
{
    return size == kMsxSubRomExactSize;
}

bool msx_compute_md5_hex(const uint8_t* data, size_t size, char out[33])
{
    if (!data || !out) {
        return false;
    }

    unsigned char digest[16] = {0};
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);

    bool ok = false;
    if (mbedtls_md5_starts_ret(&ctx) == 0 &&
        mbedtls_md5_update_ret(&ctx, data, size) == 0 &&
        mbedtls_md5_finish_ret(&ctx, digest) == 0) {
        static const char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(digest); ++i) {
            out[i * 2] = kHex[(digest[i] >> 4) & 0x0F];
            out[i * 2 + 1] = kHex[digest[i] & 0x0F];
        }
        out[32] = '\0';
        ok = true;
    }

    mbedtls_md5_free(&ctx);
    return ok;
}

uint8_t* msx_alloc_image_buffer(size_t size)
{
    uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    if (!buffer) {
        buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!buffer) {
        buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL));
    }
    if (!buffer) {
        buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_DEFAULT));
    }
    return buffer;
}

bool msx_load_file_exact(const char* path,
                         uint8_t** outData,
                         size_t* outSize,
                         char outMd5[33],
                         char* error,
                         size_t errorSize,
                         uint8_t* staticFallback,
                         size_t staticFallbackSize,
                         bool* staticFallbackUsed)
{
    if (outData) {
        *outData = nullptr;
    }
    if (outSize) {
        *outSize = 0;
    }
    if (outMd5) {
        outMd5[0] = '\0';
    }
    if (error && errorSize > 0) {
        error[0] = '\0';
    }

    if (!path || path[0] == '\0') {
        return false;
    }

    const char* openPath = msx_sd_open_path(path);
    File file = SD.open(openPath, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "missing %s", openPath);
        }
        return false;
    }

    const size_t size = static_cast<size_t>(file.size());
    if (size == 0) {
        file.close();
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "empty %s", openPath);
        }
        return false;
    }

    uint8_t* buffer = msx_alloc_image_buffer(size);
    bool usedStaticFallback = false;
    if (!buffer) {
        if (staticFallback && staticFallbackUsed &&
            size == staticFallbackSize && !(*staticFallbackUsed)) {
            buffer = staticFallback;
            *staticFallbackUsed = true;
            usedStaticFallback = true;
        }
    }
    if (!buffer) {
        file.close();
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "no heap for %s", openPath);
        }
        return false;
    }

    const size_t readBytes = file.read(buffer, size);
    file.close();

    if (readBytes != size) {
        if (usedStaticFallback && staticFallbackUsed) {
            *staticFallbackUsed = false;
        } else {
            heap_caps_free(buffer);
        }
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "short read %s", openPath);
        }
        return false;
    }

    if (outMd5 && !msx_compute_md5_hex(buffer, size, outMd5)) {
        if (usedStaticFallback && staticFallbackUsed) {
            *staticFallbackUsed = false;
        } else {
            heap_caps_free(buffer);
        }
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "md5 failed %s", openPath);
        }
        return false;
    }

    if (outData) {
        *outData = buffer;
    }
    if (outSize) {
        *outSize = size;
    }
    return true;
}

void msx_release_image(MsxBiosImage* image)
{
    if (!image) {
        return;
    }

    if (image->data && msx_media_is_static_main_bios_pointer(image->data)) {
        s_msxMainBiosStaticUsed = false;
    } else if (image->data && image->ownsData) {
        heap_caps_free(image->data);
    }

    std::memset(image, 0, sizeof(*image));
    image->status = MsxImageLoadStatus::Missing;
}

void msx_set_image_probe_details(MsxBiosImage* image,
                                 const CandidateEntry& candidate,
                                 const char* foundMd5)
{
    if (!image) {
        return;
    }

    msx_copy_string(image->path, sizeof(image->path), candidate.path);
    msx_copy_string(image->expectedName, sizeof(image->expectedName), candidate.expectedName);
    msx_copy_string(image->expectedMd5, sizeof(image->expectedMd5), candidate.expectedMd5);
    msx_copy_string(image->foundMd5, sizeof(image->foundMd5), foundMd5);
}

void msx_reset_bios_bundle(MsxBiosBundle* bundle)
{
    if (!bundle) {
        return;
    }

    msx_release_image(&bundle->mainRom);
    msx_release_image(&bundle->subRom);
    bundle->target = MsxBiosTarget::None;
    bundle->compatible = false;
    bundle->subRomRequired = false;
    bundle->message[0] = '\0';
}

bool msx_try_candidates(MsxBiosImage* image,
                        const CandidateList& candidates,
                        bool (*validate)(size_t),
                        const char* missingMessage,
                        char* detailMessage,
                        size_t detailMessageSize)
{
    if (!image) {
        return false;
    }

    msx_release_image(image);
    if (detailMessage && detailMessageSize > 0) {
        detailMessage[0] = '\0';
    }

    char lastDetail[128] = {0};
    bool foundAnyFile = false;

    for (size_t i = 0; i < candidates.count; ++i) {
        const CandidateEntry& candidate = candidates.entries[i];
        uint8_t* data = nullptr;
        size_t size = 0;
        char md5Hex[33] = {0};
        char fileError[96] = {0};
        const bool allowStaticMainBios = candidate.expectedName &&
                                         (std::strcmp(candidate.expectedName, kMsx1BiosName) == 0);

        MSX_BIOS_LOG("[MSX][BIOS] probe path=%s open=%s expected=%s\n",
                     candidate.path,
                     msx_sd_open_path(candidate.path),
                     candidate.expectedName ? candidate.expectedName : "-");
        if (!msx_load_file_exact(candidate.path,
                                 &data,
                                 &size,
                                 md5Hex,
                                 fileError,
                                 sizeof(fileError),
                                 allowStaticMainBios ? msx_main_bios_static_buffer() : nullptr,
                                 allowStaticMainBios ? kMsxMainBiosStaticSize : 0u,
                                 allowStaticMainBios ? &s_msxMainBiosStaticUsed : nullptr)) {
            MSX_BIOS_LOG("[MSX][BIOS] skip path=%s reason=%s\n", candidate.path, fileError);
            msx_copy_string(lastDetail, sizeof(lastDetail), fileError);
            continue;
        }

        foundAnyFile = true;
        MSX_BIOS_LOG("[MSX][BIOS] read path=%s open=%s size=%u md5=%s expected=%s\n",
                     candidate.path,
                     msx_sd_open_path(candidate.path),
                     static_cast<unsigned>(size),
                     md5Hex,
                     candidate.expectedMd5 ? candidate.expectedMd5 : "-");
        if (!validate(size)) {
            heap_caps_free(data);
            image->status = MsxImageLoadStatus::Incompatible;
            msx_set_image_probe_details(image, candidate, md5Hex);
            MSX_BIOS_LOG("[MSX][BIOS] reject path=%s size=%u reason=size-invalid\n",
                         candidate.path,
                         static_cast<unsigned>(size));
            std::snprintf(lastDetail,
                          sizeof(lastDetail),
                          "%s size invalid",
                          candidate.expectedName ? candidate.expectedName : "BIOS");
            continue;
        }

        if (candidate.expectedMd5 && candidate.expectedMd5[0] != '\0' &&
            std::strcmp(md5Hex, candidate.expectedMd5) != 0) {
            heap_caps_free(data);
            image->status = MsxImageLoadStatus::Incompatible;
            msx_set_image_probe_details(image, candidate, md5Hex);
            MSX_BIOS_LOG("[MSX][BIOS] reject path=%s reason=md5-mismatch expected-name=%s got=%s expected=%s\n",
                         candidate.path,
                         candidate.expectedName ? candidate.expectedName : "BIOS",
                         md5Hex,
                         candidate.expectedMd5 ? candidate.expectedMd5 : "-");
            std::snprintf(lastDetail,
                          sizeof(lastDetail),
                          "%s MD5 mismatch",
                          candidate.expectedName ? candidate.expectedName : "BIOS");
            continue;
        }

        image->data = data;
        image->size = size;
        image->status = MsxImageLoadStatus::Loaded;
        image->ownsData = !msx_media_is_static_main_bios_pointer(data);
        msx_set_image_probe_details(image, candidate, md5Hex);
        MSX_BIOS_LOG("[MSX][BIOS] accept path=%s size=%u md5=%s\n",
                     candidate.path,
                     static_cast<unsigned>(size),
                     md5Hex);
        return true;
    }

    image->status = foundAnyFile ? MsxImageLoadStatus::Incompatible : MsxImageLoadStatus::Missing;
    if (detailMessage && detailMessageSize > 0) {
        if (lastDetail[0] != '\0') {
            msx_copy_string(detailMessage, detailMessageSize, lastDetail);
        } else {
            msx_copy_string(detailMessage, detailMessageSize, missingMessage);
        }
    }
    return false;
}

bool msx_load_embedded_cbios_msx1(MsxBiosImage* image,
                                  char* detailMessage,
                                  size_t detailMessageSize)
{
    if (!image) {
        return false;
    }

    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(cbios_main_msx1_rom_start);
    const uintptr_t endAddr = reinterpret_cast<uintptr_t>(cbios_main_msx1_rom_end);
    const size_t size = (endAddr > startAddr) ? static_cast<size_t>(endAddr - startAddr) : 0u;
    if (!msx_is_valid_main_bios_size(size)) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "embedded C-BIOS size invalid");
        }
        MSX_BIOS_LOG("[MSX][BIOS] embedded C-BIOS reject size=%u\n",
                     static_cast<unsigned>(size));
        return false;
    }

    uint8_t* data = const_cast<uint8_t*>(cbios_main_msx1_rom_start);
    MSX_BIOS_LOG("[MSX][BIOS] embedded C-BIOS using flash-backed image size=%u\n",
                 static_cast<unsigned>(size));

    char md5Hex[33] = {0};
    if (!msx_compute_md5_hex(data, size, md5Hex)) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "embedded C-BIOS md5 failed");
        }
        return false;
    }

    msx_release_image(image);
    image->data = data;
    image->size = size;
    image->status = MsxImageLoadStatus::Loaded;
    image->ownsData = false;
    msx_copy_string(image->path, sizeof(image->path), kMsxEmbeddedCbiosPath);
    msx_copy_string(image->expectedName, sizeof(image->expectedName), kMsxEmbeddedCbiosName);
    image->expectedMd5[0] = '\0';
    msx_copy_string(image->foundMd5, sizeof(image->foundMd5), md5Hex);

    if (detailMessage && detailMessageSize > 0) {
        std::snprintf(detailMessage, detailMessageSize, "Embedded C-BIOS loaded");
    }
    MSX_BIOS_LOG("[MSX][BIOS] accept embedded C-BIOS size=%u md5=%s\n",
                 static_cast<unsigned>(size),
                 md5Hex);
    return true;
}

bool msx_load_embedded_cbios_msx2_subrom(MsxBiosImage* image,
                                         char* detailMessage,
                                         size_t detailMessageSize)
{
    if (!image) {
        return false;
    }

    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(cbios_sub_rom_start);
    const uintptr_t endAddr = reinterpret_cast<uintptr_t>(cbios_sub_rom_end);
    const size_t size = (endAddr > startAddr) ? static_cast<size_t>(endAddr - startAddr) : 0u;
    if (!msx_is_valid_subrom_size(size)) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "embedded MSX2 sub-ROM size invalid");
        }
        MSX_BIOS_LOG("[MSX][BIOS] embedded MSX2 sub-ROM reject size=%u\n",
                     static_cast<unsigned>(size));
        return false;
    }

    uint8_t* data = const_cast<uint8_t*>(cbios_sub_rom_start);
    MSX_BIOS_LOG("[MSX][BIOS] embedded MSX2 sub-ROM using flash-backed image size=%u\n",
                 static_cast<unsigned>(size));

    char md5Hex[33] = {0};
    if (!msx_compute_md5_hex(data, size, md5Hex)) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "embedded MSX2 sub-ROM md5 failed");
        }
        return false;
    }

    msx_release_image(image);
    image->data = data;
    image->size = size;
    image->status = MsxImageLoadStatus::Loaded;
    image->ownsData = false;
    msx_copy_string(image->path, sizeof(image->path), kMsxEmbeddedCbiosSubPath);
    msx_copy_string(image->expectedName, sizeof(image->expectedName), kMsxEmbeddedCbiosSubName);
    image->expectedMd5[0] = '\0';
    msx_copy_string(image->foundMd5, sizeof(image->foundMd5), md5Hex);

    if (detailMessage && detailMessageSize > 0) {
        std::snprintf(detailMessage, detailMessageSize, "Embedded MSX2 sub-ROM loaded");
    }
    MSX_BIOS_LOG("[MSX][BIOS] accept embedded MSX2 sub-ROM size=%u md5=%s\n",
                 static_cast<unsigned>(size),
                 md5Hex);
    return true;
}

size_t msx_find_header_offset(const uint8_t* data, size_t size)
{
    const size_t probeLimit = size < kMsxMaxHeaderProbe ? size : kMsxMaxHeaderProbe;
    for (size_t offset = 0; offset + 16 <= probeLimit; offset += kMsxHeaderStride) {
        if (msx_has_ab_signature_at(data, probeLimit, offset)) {
            return offset;
        }
    }

    // Some ROM dumps include a short wrapper ahead of the actual MSX image.
    // When the canonical 8k-aligned probe fails, do a bytewise fallback scan
    // but only accept headers whose vectors still look like a real cartridge.
    for (size_t offset = 0; offset + 16 <= probeLimit; ++offset) {
        if (msx_is_plausible_header_at(data, probeLimit, offset)) {
            return offset;
        }
    }

    return size;
}

MsxCartridgeType msx_detect_mapper_heuristic(const uint8_t* data, size_t size)
{
    unsigned ascii8Hits = 0;
    unsigned ascii16Hits = 0;
    unsigned konamiHits = 0;
    unsigned konamiSccHits = 0;

    for (size_t i = 0; i + 2 < size; ++i) {
        if (data[i] != 0x32) {
            continue;
        }

        const uint16_t address = static_cast<uint16_t>(data[i + 1] | (static_cast<uint16_t>(data[i + 2]) << 8));
        switch (address) {
            case 0x5000:
            case 0x7000:
            case 0x9000:
            case 0xB000:
                ++konamiSccHits;
                if (address == 0x7000) {
                    ++ascii16Hits;
                }
                break;
            case 0x6000:
                ++ascii8Hits;
                ++ascii16Hits;
                ++konamiHits;
                break;
            case 0x6800:
            case 0x7800:
                ++ascii8Hits;
                break;
            case 0x8000:
            case 0xA000:
                ++konamiHits;
                break;
            default:
                break;
        }
    }

    if (konamiSccHits >= 2 && konamiSccHits > ascii8Hits && konamiSccHits >= konamiHits) {
        return MsxCartridgeType::KonamiScc;
    }
    if (ascii8Hits >= 3 && ascii8Hits > ascii16Hits && ascii8Hits >= konamiHits) {
        return MsxCartridgeType::Ascii8;
    }
    if (ascii16Hits >= 2 && ascii16Hits >= konamiHits) {
        return MsxCartridgeType::Ascii16;
    }
    if (konamiHits >= 2) {
        return MsxCartridgeType::Konami;
    }

    return MsxCartridgeType::Unknown;
}

bool msx_load_for_target(MsxBiosBundle* bundle, MsxBiosTarget target, const MsxBiosSearchConfig* config)
{
    if (!bundle || !config) {
        return false;
    }

    msx_reset_bios_bundle(bundle);
    bundle->target = target;
    bundle->subRomRequired = (target == MsxBiosTarget::MSX2);

    if (target == MsxBiosTarget::MSX2 && emu_static_pool_acquire()) {
        MSX_BIOS_LOG("[MSX][BIOS] static pool prealloc=ok size=%u\n",
                     static_cast<unsigned>(EMU_STATIC_POOL_SIZE));
    }

    CandidateList mainCandidates = {};
    char detailMessage[128] = {0};

    MSX_BIOS_LOG("[MSX][BIOS] begin target=%s requested-mode=%u\n",
                 target == MsxBiosTarget::MSX2 ? "MSX2" : "MSX1",
                 static_cast<unsigned>(config->requestedMode));

    if (target == MsxBiosTarget::MSX2) {
        msx_append_candidate(&mainCandidates, config->msx2BiosPath, kMsx2BiosName, kMsx2BiosMd5);
        msx_append_candidate(&mainCandidates, config->genericBiosPath, kMsx2BiosName, kMsx2BiosMd5);
        msx_append_candidate(&mainCandidates, "/sd/bios/msx/MSX2.ROM", kMsx2BiosName, kMsx2BiosMd5);
        msx_append_candidate(&mainCandidates, "/sd/msx/MSX2.ROM", kMsx2BiosName, kMsx2BiosMd5);
    } else {
        msx_append_candidate(&mainCandidates, config->msx1BiosPath, kMsx1BiosName, kMsx1BiosMd5);
        msx_append_candidate(&mainCandidates, config->genericBiosPath, kMsx1BiosName, kMsx1BiosMd5);
        msx_append_candidate(&mainCandidates, "/sd/bios/msx/MSX.ROM", kMsx1BiosName, kMsx1BiosMd5);
        msx_append_candidate(&mainCandidates, "/sd/msx/MSX.ROM", kMsx1BiosName, kMsx1BiosMd5);
    }

    if (target == MsxBiosTarget::MSX1) {
        const bool ok = msx_try_candidates(&bundle->mainRom,
                                           mainCandidates,
                                           msx_is_valid_main_bios_size,
                                           "MSX.ROM not found",
                                           detailMessage,
                                           sizeof(detailMessage));
        if (ok) {
            bundle->compatible = true;
            msx_set_message(bundle, "MSX1 BIOS loaded");
            return true;
        }

        if (msx_load_embedded_cbios_msx1(&bundle->mainRom, detailMessage, sizeof(detailMessage))) {
            bundle->compatible = true;
            msx_set_message(bundle, "MSX1 C-BIOS fallback loaded");
            return true;
        }

        msx_set_message(bundle, detailMessage[0] != '\0' ? detailMessage : "MSX.ROM not found");
        return false;
    }

    const bool ok = msx_try_candidates(&bundle->mainRom,
                                       mainCandidates,
                                       msx_is_valid_main_bios_size,
                                       "MSX2.ROM not found",
                                       detailMessage,
                                       sizeof(detailMessage));
    if (!ok) {
        msx_set_message(bundle, detailMessage[0] != '\0' ? detailMessage : "MSX2.ROM not found");
        return false;
    }

    CandidateList subCandidates = {};
    char subDetailMessage[128] = {0};
    msx_append_candidate(&subCandidates, config->msx2SubRomPath, kMsx2ExtBiosName, kMsx2ExtBiosMd5);
    msx_append_candidate(&subCandidates, "/sd/bios/msx/MSX2EXT.ROM", kMsx2ExtBiosName, kMsx2ExtBiosMd5);
    msx_append_candidate(&subCandidates, "/sd/msx/MSX2EXT.ROM", kMsx2ExtBiosName, kMsx2ExtBiosMd5);
    if (msx_try_candidates(&bundle->subRom,
                          subCandidates,
                          msx_is_valid_subrom_size,
                          "MSX2EXT.ROM not found",
                          subDetailMessage,
                          sizeof(subDetailMessage))) {
        bundle->compatible = true;
        msx_set_message(bundle, "MSX2 BIOS and sub-ROM loaded");
        return true;
    }

    if (msx_load_embedded_cbios_msx2_subrom(&bundle->subRom, subDetailMessage, sizeof(subDetailMessage))) {
        bundle->compatible = true;
        msx_set_message(bundle, "MSX2 BIOS and embedded sub-ROM loaded");
        return true;
    }

    msx_set_message(bundle, subDetailMessage[0] != '\0' ? subDetailMessage : "MSX2EXT.ROM not found");
    return false;
}

} // namespace

bool msx_media_analyze_rom(MsxRomImage* image, const uint8_t* romData, size_t romLen)
{
    if (!image) {
        return false;
    }

    std::memset(image, 0, sizeof(*image));
    if (!romData || romLen == 0) {
        return false;
    }

    image->data = romData;
    image->size = romLen;
    image->bankCount8K = static_cast<uint8_t>((romLen + (kMsxPageSize8K - 1)) / kMsxPageSize8K);
    image->sizeSupported = (romLen >= kMsxPageSize8K) && ((romLen % kMsxPageSize8K) == 0);
    image->headerOffset = msx_find_header_offset(romData, romLen);
    image->hasAbHeader = image->headerOffset < romLen;

    if (image->hasAbHeader && image->headerOffset + 10 <= romLen) {
        const size_t offset = image->headerOffset;
        // MSX cartridge headers store the startup vector in INIT at +2.
        const uint16_t init = static_cast<uint16_t>(romData[offset + 2] | (static_cast<uint16_t>(romData[offset + 3]) << 8));
        image->entryPoint = init;
        image->initAddress = init;
    }

    if (romLen <= 0x4000) {
        image->cartridgeType = MsxCartridgeType::Plain16K;
    }
    else if (romLen <= 0x8000) {
        image->cartridgeType = MsxCartridgeType::Plain32K;
    }
    else {
        image->cartridgeType = msx_detect_mapper_heuristic(romData, romLen);
        // If heuristic cannot determine the mapper type, fall back to Konami —
        // the most common MSX mapper for >32KB cartridges.
        if (image->cartridgeType == MsxCartridgeType::Unknown) {
            image->cartridgeType = MsxCartridgeType::Konami;
        }
    }

    MSX_BIOS_LOG("[MSX][ROM] analyze size=%u header=%s offset=%u init=%04X type=%s\n",
                 static_cast<unsigned>(romLen),
                 image->hasAbHeader ? "yes" : "no",
                 image->hasAbHeader ? static_cast<unsigned>(image->headerOffset) : 0u,
                 static_cast<unsigned>(image->initAddress),
                 msx_media_cartridge_type_label(image->cartridgeType));

    return image->sizeSupported;
}

bool msx_media_load_bios_bundle(MsxBiosBundle* bundle, const MsxBiosSearchConfig* config)
{
    if (!bundle || !config) {
        return false;
    }

    msx_media_release_bios_bundle(bundle);

    if (config->requestedMode == MsxMachineMode::MSX1) {
        return msx_load_for_target(bundle, MsxBiosTarget::MSX1, config);
    }
    if (config->requestedMode == MsxMachineMode::MSX2) {
        return msx_load_for_target(bundle, MsxBiosTarget::MSX2, config);
    }

    if (msx_load_for_target(bundle, MsxBiosTarget::MSX2, config)) {
        return true;
    }

    return msx_load_for_target(bundle, MsxBiosTarget::MSX1, config);
}

void msx_media_release_bios_bundle(MsxBiosBundle* bundle)
{
    if (!bundle) {
        return;
    }

    msx_reset_bios_bundle(bundle);
    emu_static_pool_release();
}

const char* msx_media_cartridge_type_label(MsxCartridgeType type)
{
    switch (type) {
        case MsxCartridgeType::Plain16K:
            return "PLAIN16";
        case MsxCartridgeType::Plain32K:
            return "PLAIN32";
        case MsxCartridgeType::Ascii8:
            return "ASCII8";
        case MsxCartridgeType::Ascii16:
            return "ASCII16";
        case MsxCartridgeType::Konami:
            return "KONAMI";
        case MsxCartridgeType::KonamiScc:
            return "KONAMI+SCC";
        default:
            return "UNKNOWN";
    }
}

const char* msx_media_bios_target_label(MsxBiosTarget target)
{
    switch (target) {
        case MsxBiosTarget::MSX2:
            return "MSX2";
        case MsxBiosTarget::MSX1:
            return "MSX1";
        default:
            return "UNKNOWN";
    }
}

MsxMachineMode msx_media_target_to_machine_mode(MsxBiosTarget target)
{
    switch (target) {
        case MsxBiosTarget::MSX2:
            return MsxMachineMode::MSX2;
        case MsxBiosTarget::MSX1:
            return MsxMachineMode::MSX1;
        default:
            return MsxMachineMode::Auto;
    }
}

bool msx_media_is_static_main_bios_pointer(const uint8_t* data)
{
    return data == msx_main_bios_static_buffer_if_acquired();
}

uint8_t msx_media_static_ram_bank_count_for_main_bios(const uint8_t* mainRom)
{
    if (!emu_static_pool_acquire()) {
        return 0u;
    }

    const size_t reservedBytes = msx_media_is_static_main_bios_pointer(mainRom)
                                     ? kMsxMainBiosStaticSize
                                     : 0u;
    return static_cast<uint8_t>((EMU_STATIC_POOL_SIZE - reservedBytes) / kMsxPageSize8K);
}

uint8_t* msx_media_static_ram_bank_ptr_for_main_bios(const uint8_t* mainRom, uint8_t bankIndex)
{
    if (!emu_static_pool_acquire()) {
        return nullptr;
    }

    const uint8_t staticBankCount = msx_media_static_ram_bank_count_for_main_bios(mainRom);
    if (bankIndex >= staticBankCount) {
        return nullptr;
    }

    return emu_static_pool_get() + (static_cast<size_t>(bankIndex) * kMsxPageSize8K);
}
