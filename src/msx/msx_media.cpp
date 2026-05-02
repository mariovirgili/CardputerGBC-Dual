#include "msx_media.h"

#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../share/emu_static_pool.h"

extern const uint8_t cbios_main_msx1_rom_start[] asm("_binary_bios_cbios_0_29a_roms_cbios_main_msx1_rom_start");
extern const uint8_t cbios_main_msx1_rom_end[] asm("_binary_bios_cbios_0_29a_roms_cbios_main_msx1_rom_end");
extern const uint8_t cbios_sub_rom_start[] asm("_binary_bios_cbios_0_29a_roms_cbios_sub_rom_start");
extern const uint8_t cbios_sub_rom_end[] asm("_binary_bios_cbios_0_29a_roms_cbios_sub_rom_end");
#if MSX_EMBED_OFFICIAL_MSX2
extern const uint8_t official_msx2_rom_start[] asm("_binary_bios_private_MSX2_ROM_start");
extern const uint8_t official_msx2_rom_end[] asm("_binary_bios_private_MSX2_ROM_end");
#endif
#if MSX_EMBED_OFFICIAL_MSX2EXT
extern const uint8_t official_msx2ext_rom_start[] asm("_binary_bios_private_MSX2EXT_ROM_start");
extern const uint8_t official_msx2ext_rom_end[] asm("_binary_bios_private_MSX2EXT_ROM_end");
#endif

#ifndef MSX_BIOS_LOG_ENABLED
#define MSX_BIOS_LOG_ENABLED 1
#endif

#ifndef MSX_EMBED_OFFICIAL_MSX2EXT
#define MSX_EMBED_OFFICIAL_MSX2EXT 0
#endif

#ifndef MSX_EMBED_OFFICIAL_MSX2
#define MSX_EMBED_OFFICIAL_MSX2 0
#endif

#ifndef MSX_MSX2_FLASH_PARTITION
#define MSX_MSX2_FLASH_PARTITION 0
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
constexpr const char* kMsxEmbeddedOfficialMainPath = "[embedded]/MSX2.ROM";
constexpr const char* kMsxEmbeddedOfficialSubPath = "[embedded]/MSX2EXT.ROM";
constexpr const char* kMsxFlashOfficialMainPath = "[flash]/MSX2.ROM";
constexpr const char* kMsxFlashOfficialSubPath = "[flash]/MSX2EXT.ROM";
constexpr const char* kMsx2FlashPartitionLabel = "msx2bios";
constexpr size_t kMsx2FlashMainOffset = 0x0000;
constexpr size_t kMsx2FlashSubOffset = 0x8000;
constexpr size_t kMsx2FlashPartitionSize = 0xC000;
constexpr const char* kMsx2FlashMainMd5 = "ec3a01c91f24fbddcbcab0ad301bc9ef";
constexpr const char* kMsx2FlashSubMd5 = "2183c2aff17cf4297bdb496de78c2e8a";
constexpr size_t kMsxMainBiosStaticOffset = EMU_STATIC_POOL_SIZE - kMsxMainBiosStaticSize;
constexpr const char* kMsxCockpitAscii16BootMirrorSha1 =
    "e36e16acfdfa76fa72b218da2bebc668db39d21e";

static_assert(EMU_STATIC_POOL_SIZE >= (kMsxMainBiosStaticSize + kMsxPageSize8K),
              "MSX static pool too small for official BIOS fallback");

static bool s_msxMainBiosStaticUsed = false;
static spi_flash_mmap_handle_t s_msx2BiosFlashMmap = 0;
static const uint8_t* s_msx2BiosFlashData = nullptr;

struct MsxFmsxMapperShaEntry {
    const char sha1[41];
    uint8_t mapperType;
};

static constexpr MsxFmsxMapperShaEntry kMsxFmsxMapperShaTable[] = {
#include "msx_mapper_db.inc"
};

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

bool msx_compute_sha1_hex(const uint8_t* data, size_t size, char out[41])
{
    if (!data || !out) {
        return false;
    }

    unsigned char digest[20] = {0};
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);

    bool ok = false;
    if (mbedtls_sha1_starts_ret(&ctx) == 0 &&
        mbedtls_sha1_update_ret(&ctx, data, size) == 0 &&
        mbedtls_sha1_finish_ret(&ctx, digest) == 0) {
        static const char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(digest); ++i) {
            out[i * 2] = kHex[(digest[i] >> 4) & 0x0F];
            out[i * 2 + 1] = kHex[digest[i] & 0x0F];
        }
        out[40] = '\0';
        ok = true;
    }

    mbedtls_sha1_free(&ctx);
    return ok;
}

bool msx_sha1_matches(const uint8_t* data, size_t size, const char* expectedSha1)
{
    if (!data || !expectedSha1) {
        return false;
    }

    char sha1Hex[41] = {0};
    return msx_compute_sha1_hex(data, size, sha1Hex) &&
           std::strcmp(sha1Hex, expectedSha1) == 0;
}

bool msx_md5_finish_hex(mbedtls_md5_context* ctx, char out[33])
{
    if (!ctx || !out) {
        return false;
    }

    unsigned char digest[16] = {0};
    if (mbedtls_md5_finish_ret(ctx, digest) != 0) {
        out[0] = '\0';
        return false;
    }

    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out[i * 2] = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    out[32] = '\0';
    return true;
}

bool msx_media_is_flash_msx2bios_pointer(const uint8_t* data)
{
    if (!data || !s_msx2BiosFlashData) {
        return false;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(data);
    const uintptr_t start = reinterpret_cast<uintptr_t>(s_msx2BiosFlashData);
    return address >= start && address < (start + kMsx2FlashPartitionSize);
}

void msx_release_flash_msx2bios_map(void)
{
    if (s_msx2BiosFlashMmap) {
        spi_flash_munmap(s_msx2BiosFlashMmap);
        s_msx2BiosFlashMmap = 0;
    }
    s_msx2BiosFlashData = nullptr;
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
    } else if (image->data && msx_media_is_flash_msx2bios_pointer(image->data)) {
        // Shared mmap for MSX2.ROM + MSX2EXT.ROM is released with the bundle.
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
    msx_release_flash_msx2bios_map();
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

bool msx_partition_region_md5_hex(const esp_partition_t* part, size_t offset, size_t size, char out[33])
{
#if !MSX_MSX2_FLASH_PARTITION
    (void)part;
    (void)offset;
    (void)size;
    (void)out;
    return false;
#else
    if (!part || !out || size == 0u || offset > part->size || size > (part->size - offset)) {
        return false;
    }

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    bool ok = mbedtls_md5_starts_ret(&ctx) == 0;
    uint8_t buffer[512];
    size_t consumed = 0u;
    while (ok && consumed < size) {
        size_t chunk = size - consumed;
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        if (esp_partition_read(part, offset + consumed, buffer, chunk) != ESP_OK ||
            mbedtls_md5_update_ret(&ctx, buffer, chunk) != 0) {
            ok = false;
            break;
        }
        consumed += chunk;
    }
    if (ok) {
        ok = msx_md5_finish_hex(&ctx, out);
    }
    mbedtls_md5_free(&ctx);
    return ok;
#endif
}

void msx_set_flash_msx2_image(MsxBiosImage* image,
                              const uint8_t* data,
                              size_t size,
                              const char* path,
                              const char* expectedName,
                              const char* expectedMd5,
                              const char* foundMd5)
{
    if (!image || !data) {
        return;
    }

    msx_release_image(image);
    image->data = const_cast<uint8_t*>(data);
    image->size = size;
    image->status = MsxImageLoadStatus::Loaded;
    image->ownsData = false;
    msx_copy_string(image->path, sizeof(image->path), path);
    msx_copy_string(image->expectedName, sizeof(image->expectedName), expectedName);
    msx_copy_string(image->expectedMd5, sizeof(image->expectedMd5), expectedMd5);
    msx_copy_string(image->foundMd5, sizeof(image->foundMd5), foundMd5 ? foundMd5 : "");
}

bool msx_map_flash_msx2bios_partition(const esp_partition_t* part,
                                      MsxBiosBundle* bundle,
                                      const char* mainMd5,
                                      const char* subMd5,
                                      char* detailMessage,
                                      size_t detailMessageSize)
{
#if !MSX_MSX2_FLASH_PARTITION
    (void)part;
    (void)bundle;
    (void)mainMd5;
    (void)subMd5;
    (void)detailMessage;
    (void)detailMessageSize;
    return false;
#else
    if (!part || !bundle || part->size < kMsx2FlashPartitionSize) {
        return false;
    }

    msx_release_flash_msx2bios_map();
    const void* ptr = nullptr;
    if (esp_partition_mmap(part,
                           0,
                           kMsx2FlashPartitionSize,
                           SPI_FLASH_MMAP_DATA,
                           &ptr,
                           &s_msx2BiosFlashMmap) != ESP_OK ||
        !ptr) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "MSX2 BIOS flash mmap failed");
        }
        s_msx2BiosFlashMmap = 0;
        return false;
    }

    s_msx2BiosFlashData = static_cast<const uint8_t*>(ptr);
    msx_set_flash_msx2_image(&bundle->mainRom,
                             s_msx2BiosFlashData + kMsx2FlashMainOffset,
                             kMsxMainBiosStaticSize,
                             kMsxFlashOfficialMainPath,
                             kMsx2BiosName,
                             kMsx2FlashMainMd5,
                             mainMd5);
    msx_set_flash_msx2_image(&bundle->subRom,
                             s_msx2BiosFlashData + kMsx2FlashSubOffset,
                             kMsxSubRomExactSize,
                             kMsxFlashOfficialSubPath,
                             kMsx2ExtBiosName,
                             kMsx2FlashSubMd5,
                             subMd5);

    if (detailMessage && detailMessageSize > 0) {
        std::snprintf(detailMessage, detailMessageSize, "MSX2 BIOS flash cache loaded");
    }
    MSX_BIOS_LOG("[MSX][BIOS] accept flash MSX2 BIOS partition main=%s sub=%s\n",
                 mainMd5 ? mainMd5 : "-",
                 subMd5 ? subMd5 : "-");
    return true;
#endif
}

bool msx_install_msx2_flash_region_from_file(const char* path,
                                             const esp_partition_t* part,
                                             size_t partitionOffset,
                                             size_t expectedSize,
                                             const char* expectedMd5,
                                             const char* expectedName,
                                             char outMd5[33],
                                             char* detailMessage,
                                             size_t detailMessageSize)
{
#if !MSX_MSX2_FLASH_PARTITION
    (void)path;
    (void)part;
    (void)partitionOffset;
    (void)expectedSize;
    (void)expectedMd5;
    (void)expectedName;
    (void)outMd5;
    (void)detailMessage;
    (void)detailMessageSize;
    return false;
#else
    if (outMd5) {
        outMd5[0] = '\0';
    }
    if (!path || !part || !expectedMd5 || !expectedName ||
        partitionOffset > part->size || expectedSize > (part->size - partitionOffset)) {
        return false;
    }

    const char* openPath = msx_sd_open_path(path);
    File file = SD.open(openPath, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "missing %s", openPath);
        }
        return false;
    }

    const size_t size = static_cast<size_t>(file.size());
    if (size != expectedSize) {
        file.close();
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "%s size invalid", expectedName);
        }
        return false;
    }

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    bool ok = mbedtls_md5_starts_ret(&ctx) == 0;
    uint8_t buffer[512];
    size_t written = 0u;
    while (ok && written < size) {
        size_t chunk = size - written;
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        const size_t readBytes = file.read(buffer, chunk);
        if (readBytes != chunk ||
            mbedtls_md5_update_ret(&ctx, buffer, chunk) != 0 ||
            esp_partition_write(part, partitionOffset + written, buffer, chunk) != ESP_OK) {
            ok = false;
            break;
        }
        written += chunk;
    }
    file.close();

    char md5Hex[33] = {0};
    if (ok) {
        ok = msx_md5_finish_hex(&ctx, md5Hex);
    }
    mbedtls_md5_free(&ctx);

    if (!ok || written != size || std::strcmp(md5Hex, expectedMd5) != 0) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage,
                          detailMessageSize,
                          ok ? "%s MD5 mismatch" : "%s flash write failed",
                          expectedName);
        }
        MSX_BIOS_LOG("[MSX][BIOS] reject flash install path=%s md5=%s expected=%s\n",
                     path,
                     md5Hex[0] ? md5Hex : "-",
                     expectedMd5);
        return false;
    }

    if (outMd5) {
        msx_copy_string(outMd5, 33, md5Hex);
    }
    MSX_BIOS_LOG("[MSX][BIOS] installed %s flash cache from %s md5=%s\n",
                 expectedName,
                 path,
                 md5Hex);
    return true;
#endif
}

bool msx_install_msx2_flash_region_from_candidates(const CandidateList& candidates,
                                                   const esp_partition_t* part,
                                                   size_t partitionOffset,
                                                   size_t expectedSize,
                                                   const char* expectedMd5,
                                                   const char* expectedName,
                                                   char outMd5[33],
                                                   char* detailMessage,
                                                   size_t detailMessageSize)
{
#if !MSX_MSX2_FLASH_PARTITION
    (void)candidates;
    (void)part;
    (void)partitionOffset;
    (void)expectedSize;
    (void)expectedMd5;
    (void)expectedName;
    (void)outMd5;
    (void)detailMessage;
    (void)detailMessageSize;
    return false;
#else
    char lastDetail[128] = {0};
    for (size_t i = 0; i < candidates.count; ++i) {
        const CandidateEntry& candidate = candidates.entries[i];
        MSX_BIOS_LOG("[MSX][BIOS] flash-cache probe path=%s open=%s expected=%s\n",
                     candidate.path,
                     msx_sd_open_path(candidate.path),
                     expectedName);
        if (msx_install_msx2_flash_region_from_file(candidate.path,
                                                    part,
                                                    partitionOffset,
                                                    expectedSize,
                                                    expectedMd5,
                                                    expectedName,
                                                    outMd5,
                                                    lastDetail,
                                                    sizeof(lastDetail))) {
            return true;
        }
        MSX_BIOS_LOG("[MSX][BIOS] flash-cache skip path=%s reason=%s\n",
                     candidate.path,
                     lastDetail[0] ? lastDetail : "not usable");
    }

    if (detailMessage && detailMessageSize > 0) {
        msx_copy_string(detailMessage,
                        detailMessageSize,
                        lastDetail[0] ? lastDetail : "MSX2 BIOS flash source missing");
    }
    return false;
#endif
}

bool msx_load_flash_cached_official_msx2_bundle(MsxBiosBundle* bundle,
                                                const CandidateList& mainCandidates,
                                                const CandidateList& subCandidates,
                                                char* detailMessage,
                                                size_t detailMessageSize)
{
#if !MSX_MSX2_FLASH_PARTITION
    (void)bundle;
    (void)mainCandidates;
    (void)subCandidates;
    (void)detailMessage;
    (void)detailMessageSize;
    return false;
#else
    if (!bundle) {
        return false;
    }

    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_ANY,
                                                           kMsx2FlashPartitionLabel);
    if (!part || part->size < kMsx2FlashPartitionSize) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "MSX2 BIOS flash partition missing");
        }
        return false;
    }

    char mainMd5[33] = {0};
    char subMd5[33] = {0};
    if (msx_partition_region_md5_hex(part, kMsx2FlashMainOffset, kMsxMainBiosStaticSize, mainMd5) &&
        msx_partition_region_md5_hex(part, kMsx2FlashSubOffset, kMsxSubRomExactSize, subMd5) &&
        std::strcmp(mainMd5, kMsx2FlashMainMd5) == 0 &&
        std::strcmp(subMd5, kMsx2FlashSubMd5) == 0 &&
        msx_map_flash_msx2bios_partition(part, bundle, mainMd5, subMd5, detailMessage, detailMessageSize)) {
        return true;
    }

    msx_release_flash_msx2bios_map();
    if (esp_partition_erase_range(part, 0, part->size) != ESP_OK) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "MSX2 BIOS flash erase failed");
        }
        return false;
    }

    char installDetail[128] = {0};
    if (!msx_install_msx2_flash_region_from_candidates(mainCandidates,
                                                       part,
                                                       kMsx2FlashMainOffset,
                                                       kMsxMainBiosStaticSize,
                                                       kMsx2FlashMainMd5,
                                                       kMsx2BiosName,
                                                       mainMd5,
                                                       installDetail,
                                                       sizeof(installDetail)) ||
        !msx_install_msx2_flash_region_from_candidates(subCandidates,
                                                       part,
                                                       kMsx2FlashSubOffset,
                                                       kMsxSubRomExactSize,
                                                       kMsx2FlashSubMd5,
                                                       kMsx2ExtBiosName,
                                                       subMd5,
                                                       installDetail,
                                                       sizeof(installDetail))) {
        (void)esp_partition_erase_range(part, 0, part->size);
        if (detailMessage && detailMessageSize > 0) {
            msx_copy_string(detailMessage,
                            detailMessageSize,
                            installDetail[0] ? installDetail : "MSX2 BIOS flash install failed");
        }
        bundle->mainRom.status = MsxImageLoadStatus::Missing;
        bundle->subRom.status = MsxImageLoadStatus::Missing;
        return false;
    }

    if (!msx_partition_region_md5_hex(part, kMsx2FlashMainOffset, kMsxMainBiosStaticSize, mainMd5) ||
        !msx_partition_region_md5_hex(part, kMsx2FlashSubOffset, kMsxSubRomExactSize, subMd5) ||
        std::strcmp(mainMd5, kMsx2FlashMainMd5) != 0 ||
        std::strcmp(subMd5, kMsx2FlashSubMd5) != 0) {
        (void)esp_partition_erase_range(part, 0, part->size);
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "MSX2 BIOS flash verify failed");
        }
        return false;
    }

    return msx_map_flash_msx2bios_partition(part, bundle, mainMd5, subMd5, detailMessage, detailMessageSize);
#endif
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

bool msx_load_embedded_official_msx2_mainrom(MsxBiosImage* image,
                                             char* detailMessage,
                                             size_t detailMessageSize)
{
#if !MSX_EMBED_OFFICIAL_MSX2
    (void)image;
    (void)detailMessage;
    (void)detailMessageSize;
    return false;
#else
    if (!image) {
        return false;
    }

    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(official_msx2_rom_start);
    const uintptr_t endAddr = reinterpret_cast<uintptr_t>(official_msx2_rom_end);
    const size_t size = (endAddr > startAddr) ? static_cast<size_t>(endAddr - startAddr) : 0u;
    if (!msx_is_valid_main_bios_size(size)) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "embedded official MSX2 size invalid");
        }
        MSX_BIOS_LOG("[MSX][BIOS] embedded official MSX2 reject size=%u\n",
                     static_cast<unsigned>(size));
        return false;
    }

    uint8_t* data = const_cast<uint8_t*>(official_msx2_rom_start);
    MSX_BIOS_LOG("[MSX][BIOS] embedded official MSX2 using flash-backed image size=%u\n",
                 static_cast<unsigned>(size));

    char md5Hex[33] = {0};
    if (!msx_compute_md5_hex(data, size, md5Hex)) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "embedded official MSX2 md5 failed");
        }
        return false;
    }

    msx_release_image(image);
    image->data = data;
    image->size = size;
    image->status = MsxImageLoadStatus::Loaded;
    image->ownsData = false;
    msx_copy_string(image->path, sizeof(image->path), kMsxEmbeddedOfficialMainPath);
    msx_copy_string(image->expectedName, sizeof(image->expectedName), kMsx2BiosName);
    image->expectedMd5[0] = '\0';
    msx_copy_string(image->foundMd5, sizeof(image->foundMd5), md5Hex);

    if (detailMessage && detailMessageSize > 0) {
        std::snprintf(detailMessage, detailMessageSize, "Embedded official MSX2 loaded");
    }
    MSX_BIOS_LOG("[MSX][BIOS] accept embedded official MSX2 size=%u md5=%s\n",
                 static_cast<unsigned>(size),
                 md5Hex);
    return true;
#endif
}

bool msx_load_embedded_official_msx2_subrom(MsxBiosImage* image,
                                            char* detailMessage,
                                            size_t detailMessageSize)
{
#if !MSX_EMBED_OFFICIAL_MSX2EXT
    (void)image;
    (void)detailMessage;
    (void)detailMessageSize;
    return false;
#else
    if (!image) {
        return false;
    }

    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(official_msx2ext_rom_start);
    const uintptr_t endAddr = reinterpret_cast<uintptr_t>(official_msx2ext_rom_end);
    const size_t size = (endAddr > startAddr) ? static_cast<size_t>(endAddr - startAddr) : 0u;
    if (!msx_is_valid_subrom_size(size)) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "embedded official MSX2EXT size invalid");
        }
        MSX_BIOS_LOG("[MSX][BIOS] embedded official MSX2EXT reject size=%u\n",
                     static_cast<unsigned>(size));
        return false;
    }

    uint8_t* data = const_cast<uint8_t*>(official_msx2ext_rom_start);
    MSX_BIOS_LOG("[MSX][BIOS] embedded official MSX2EXT using flash-backed image size=%u\n",
                 static_cast<unsigned>(size));

    char md5Hex[33] = {0};
    if (!msx_compute_md5_hex(data, size, md5Hex)) {
        if (detailMessage && detailMessageSize > 0) {
            std::snprintf(detailMessage, detailMessageSize, "embedded official MSX2EXT md5 failed");
        }
        return false;
    }

    msx_release_image(image);
    image->data = data;
    image->size = size;
    image->status = MsxImageLoadStatus::Loaded;
    image->ownsData = false;
    msx_copy_string(image->path, sizeof(image->path), kMsxEmbeddedOfficialSubPath);
    msx_copy_string(image->expectedName, sizeof(image->expectedName), kMsx2ExtBiosName);
    image->expectedMd5[0] = '\0';
    msx_copy_string(image->foundMd5, sizeof(image->foundMd5), md5Hex);

    if (detailMessage && detailMessageSize > 0) {
        std::snprintf(detailMessage, detailMessageSize, "Embedded official MSX2EXT loaded");
    }
    MSX_BIOS_LOG("[MSX][BIOS] accept embedded official MSX2EXT size=%u md5=%s\n",
                 static_cast<unsigned>(size),
                 md5Hex);
    return true;
#endif
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

MsxCartridgeType msx_fmsx_mapper_type_to_cartridge_type(uint8_t mapperType)
{
    switch (mapperType) {
        case 2u:
            return MsxCartridgeType::KonamiScc;
        case 3u:
            return MsxCartridgeType::Konami;
        case 4u:
            return MsxCartridgeType::Ascii8;
        case 5u:
            return MsxCartridgeType::Ascii16;
        case 7u:
            return MsxCartridgeType::Fmpac;
        default:
            break;
    }
    return MsxCartridgeType::Unknown;
}

const char* msx_fmsx_mapper_type_label(uint8_t mapperType)
{
    switch (mapperType) {
        case 0u:
            return "GENERIC8";
        case 1u:
            return "GENERIC16";
        case 2u:
            return "KONAMI5/SCC";
        case 3u:
            return "KONAMI4";
        case 4u:
            return "ASCII8";
        case 5u:
            return "ASCII16";
        case 6u:
            return "GMASTER2";
        case 7u:
            return "FMPAC";
        default:
            break;
    }
    return "UNKNOWN";
}

bool msx_detect_mapper_from_fmsx_sha(const uint8_t* data,
                                     size_t size,
                                     MsxCartridgeType* outType,
                                     uint8_t* outMapperType)
{
    if (!data || size == 0u || !outType) {
        return false;
    }

    char sha1Hex[41] = {0};
    if (!msx_compute_sha1_hex(data, size, sha1Hex)) {
        return false;
    }

    for (const MsxFmsxMapperShaEntry& entry : kMsxFmsxMapperShaTable) {
        if (std::strcmp(sha1Hex, entry.sha1) != 0) {
            continue;
        }

        if (outMapperType) {
            *outMapperType = entry.mapperType;
        }
        *outType = msx_fmsx_mapper_type_to_cartridge_type(entry.mapperType);
        return true;
    }

    return false;
}

MsxCartridgeType msx_detect_mapper_heuristic(const uint8_t* data, size_t size)
{
    unsigned ascii8Hits = 0;
    unsigned ascii16Hits = 0;
    unsigned konamiHits = 0;
    unsigned konamiSccHits = 0;
    unsigned konamiSccSpecificHits = 0;

    for (size_t i = 0; i + 2 < size; ++i) {
        if (data[i] != 0x32) {
            continue;
        }

        const uint16_t address = static_cast<uint16_t>(data[i + 1] | (static_cast<uint16_t>(data[i + 2]) << 8));
        switch (address) {
            case 0x5000:
            case 0x9000:
            case 0xB000:
                ++konamiSccHits;
                ++konamiSccSpecificHits;
                break;
            case 0x7000:
                // 7000h is shared by Konami SCC and ASCII16. Some ASCII16
                // games, notably Andorogynus, mostly advertise this address.
                ++konamiSccHits;
                ++ascii16Hits;
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

    if (ascii16Hits >= 2 &&
        konamiSccSpecificHits <= 1 &&
        ascii16Hits >= (konamiSccSpecificHits + 1u) * 4u &&
        ascii16Hits >= konamiHits) {
        return MsxCartridgeType::Ascii16;
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
        msx_append_candidate(&mainCandidates, "/sd/bios/private/MSX2.ROM", kMsx2BiosName, kMsx2BiosMd5);
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

    CandidateList subCandidates = {};
    char subDetailMessage[128] = {0};
    msx_append_candidate(&subCandidates, config->msx2SubRomPath, kMsx2ExtBiosName, kMsx2ExtBiosMd5);
    msx_append_candidate(&subCandidates, "/sd/bios/private/MSX2EXT.ROM", kMsx2ExtBiosName, kMsx2ExtBiosMd5);
    msx_append_candidate(&subCandidates, "/sd/bios/msx/MSX2EXT.ROM", kMsx2ExtBiosName, kMsx2ExtBiosMd5);
    msx_append_candidate(&subCandidates, "/sd/msx/MSX2EXT.ROM", kMsx2ExtBiosName, kMsx2ExtBiosMd5);

#if MSX_MSX2_FLASH_PARTITION
    if (msx_load_flash_cached_official_msx2_bundle(bundle,
                                                   mainCandidates,
                                                   subCandidates,
                                                   detailMessage,
                                                   sizeof(detailMessage))) {
        bundle->compatible = true;
        msx_set_message(bundle, "MSX2 BIOS flash cache loaded");
        return true;
    }

    msx_set_message(bundle, detailMessage[0] != '\0' ? detailMessage : "MSX2 BIOS flash cache unavailable");
    return false;
#endif

    bool mainEmbedded = false;
    if (msx_load_embedded_official_msx2_mainrom(&bundle->mainRom,
                                                detailMessage,
                                                sizeof(detailMessage))) {
        mainEmbedded = true;
    } else if (!msx_try_candidates(&bundle->mainRom,
                                   mainCandidates,
                                   msx_is_valid_main_bios_size,
                                   "MSX2.ROM not found",
                                   detailMessage,
                                   sizeof(detailMessage))) {
        msx_set_message(bundle, detailMessage[0] != '\0' ? detailMessage : "MSX2.ROM not found");
        return false;
    }

    bool subEmbeddedOfficial = false;
    bool subEmbeddedCbios = false;
    if (msx_load_embedded_official_msx2_subrom(&bundle->subRom,
                                               subDetailMessage,
                                               sizeof(subDetailMessage))) {
        subEmbeddedOfficial = true;
    } else if (msx_try_candidates(&bundle->subRom,
                                  subCandidates,
                                  msx_is_valid_subrom_size,
                                  "MSX2EXT.ROM not found",
                                  subDetailMessage,
                                  sizeof(subDetailMessage))) {
        bundle->compatible = true;
        msx_set_message(bundle,
                        mainEmbedded ? "Embedded MSX2 BIOS and sub-ROM loaded"
                                     : "MSX2 BIOS and sub-ROM loaded");
        return true;
    } else if (msx_load_embedded_cbios_msx2_subrom(&bundle->subRom,
                                                   subDetailMessage,
                                                   sizeof(subDetailMessage))) {
        subEmbeddedCbios = true;
        bundle->compatible = true;
        msx_set_message(bundle,
                        mainEmbedded ? "Embedded MSX2 BIOS and embedded sub-ROM loaded"
                                     : "MSX2 BIOS and embedded sub-ROM loaded");
        return true;
    }

    if (subEmbeddedOfficial) {
        bundle->compatible = true;
        msx_set_message(bundle,
                        mainEmbedded ? "Embedded MSX2 BIOS and embedded MSX2EXT loaded"
                                     : "MSX2 BIOS and embedded MSX2EXT loaded");
        return true;
    }

    (void)subEmbeddedCbios;

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

    const char* mapperSource = "fixed";
    bool fmsxMapperMatched = false;
    uint8_t fmsxMapperType = 0xFFu;

    if (romLen == 0x10000u && image->headerOffset == 0x4000u) {
        image->cartridgeType = MsxCartridgeType::Plain64K;
    }
    else if (romLen <= 0x4000) {
        image->cartridgeType = MsxCartridgeType::Plain16K;
    }
    else if (romLen <= 0x8000) {
        image->cartridgeType = MsxCartridgeType::Plain32K;
    }
    else {
        MsxCartridgeType fmsxType = MsxCartridgeType::Unknown;
        fmsxMapperMatched = msx_detect_mapper_from_fmsx_sha(romData, romLen, &fmsxType, &fmsxMapperType);
        if (fmsxMapperMatched && fmsxType != MsxCartridgeType::Unknown) {
            image->cartridgeType = fmsxType;
            mapperSource = "fmsx-sha";
        } else {
            image->cartridgeType = msx_detect_mapper_heuristic(romData, romLen);
            mapperSource = fmsxMapperMatched ? "heuristic-after-unsupported-fmsx-sha" : "heuristic";
        }
        // If heuristic cannot determine the mapper type, fall back to Konami —
        // the most common MSX mapper for >32KB cartridges.
        if (image->cartridgeType == MsxCartridgeType::Unknown) {
            image->cartridgeType = MsxCartridgeType::Konami;
        }
    }

    if (image->cartridgeType == MsxCartridgeType::Ascii16 &&
        msx_sha1_matches(romData, romLen, kMsxCockpitAscii16BootMirrorSha1)) {
        image->quirks |= MsxRomQuirkAscii16BootMirror;
    }

    if (fmsxMapperMatched) {
        MSX_BIOS_LOG("[MSX][ROM] fMSX SHA mapper=%s supported=%s\n",
                     msx_fmsx_mapper_type_label(fmsxMapperType),
                     msx_fmsx_mapper_type_to_cartridge_type(fmsxMapperType) != MsxCartridgeType::Unknown ? "yes" : "no");
    }

    MSX_BIOS_LOG("[MSX][ROM] analyze size=%u header=%s offset=%u init=%04X type=%s source=%s quirks=%02X\n",
                 static_cast<unsigned>(romLen),
                 image->hasAbHeader ? "yes" : "no",
                 image->hasAbHeader ? static_cast<unsigned>(image->headerOffset) : 0u,
                 static_cast<unsigned>(image->initAddress),
                 msx_media_cartridge_type_label(image->cartridgeType),
                 mapperSource,
                 static_cast<unsigned>(image->quirks));

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
        case MsxCartridgeType::Plain64K:
            return "PLAIN64";
        case MsxCartridgeType::Ascii8:
            return "ASCII8";
        case MsxCartridgeType::Ascii16:
            return "ASCII16";
        case MsxCartridgeType::Konami:
            return "KONAMI";
        case MsxCartridgeType::KonamiScc:
            return "KONAMI+SCC";
        case MsxCartridgeType::Fmpac:
            return "FMPAC";
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
