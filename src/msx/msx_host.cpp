#pragma GCC optimize ("Os")

#include "msx_host_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "msx_save.h"

namespace msx {
HostState g_host;
}

namespace {

static void free_bios(void)
{
    for (int i = 0; i < msx::g_host.biosCount; ++i) {
        if (msx::g_host.bios[i].ownedHeap) {
            free((void*)msx::g_host.bios[i].data);
        }
        msx::g_host.bios[i].data = nullptr;
        msx::g_host.bios[i].size = 0;
        msx::g_host.bios[i].ownedHeap = false;
        msx::g_host.bios[i].name[0] = '\0';
    }
    msx::g_host.biosCount = 0;
}

static void free_game_names(void)
{
    free(msx::g_host.romName);
    free(msx::g_host.romPath);
    msx::g_host.romName = nullptr;
    msx::g_host.romPath = nullptr;
}

static char* dup_str(const char* s)
{
    if (!s || !*s) return nullptr;
    const size_t len = strlen(s) + 1;
    char* out = (char*)malloc(len);
    if (!out) return nullptr;
    memcpy(out, s, len);
    return out;
}

static const char* base_name(const char* path)
{
    if (!path) return nullptr;
    const char* slash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    const char* cut = slash > bslash ? slash : bslash;
    return cut ? (cut + 1) : path;
}

static bool bios_loaded(const char* fileName)
{
    for (int i = 0; i < msx::g_host.biosCount; ++i) {
        if (!strcasecmp(msx::g_host.bios[i].name, fileName)) {
            return true;
        }
    }
    return false;
}

static bool add_bios_from_sd(const char* fileName)
{
    if (!fileName || !*fileName) return false;
    if (bios_loaded(fileName)) return true;
    if (msx::g_host.biosCount >= (int)(sizeof(msx::g_host.bios) / sizeof(msx::g_host.bios[0]))) return false;

    static const char* const kBiosDirs[] = {
        "/sd/msx",
        "/sd/roms",
        "/sd/bios",
        "/sd"
    };

    char path[128];
    FILE* f = nullptr;

    for (unsigned i = 0; i < sizeof(kBiosDirs) / sizeof(kBiosDirs[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", kBiosDirs[i], fileName);
        f = fopen(path, "rb");
        if (f) break;
    }
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }

    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    if ((long)fread(buf, 1, (size_t)sz, f) != sz) { free(buf); fclose(f); return false; }
    fclose(f);

    msx::BiosBlob& blob = msx::g_host.bios[msx::g_host.biosCount++];
    strncpy(blob.name, fileName, sizeof(blob.name) - 1);
    blob.name[sizeof(blob.name) - 1] = '\0';
    blob.data = buf;
    blob.size = (unsigned int)sz;
    blob.ownedHeap = true;

    printf("[MSX] BIOS loaded from SD: %s (%ld bytes)\n", path, sz);
    return true;
}

static bool ensure_bios(const char* fileName)
{
    if (bios_loaded(fileName)) return true;

    if (add_bios_from_sd(fileName)) return true;

    printf("[MSX] BIOS missing: %s\n", fileName);
    return false;
}

} // namespace

extern "C" void msx_host_set_game(const uint8_t* romData, unsigned int romSize, const char* romName, const char* romPath)
{
    free_game_names();
    msx::g_host.romData = romData;
    msx::g_host.romSize = romSize;
    msx::g_host.romName = dup_str(romName);
    msx::g_host.romPath = dup_str(romPath);
    msx_save_set_game_identity(msx::g_host.romName, msx::g_host.romPath);
}

extern "C" void msx_host_clear_game(void)
{
    msx::g_host.romData = nullptr;
    msx::g_host.romSize = 0;
    msx_save_clear_game_identity();
    free_game_names();
}

extern "C" void msx_host_set_model_mode(int mode)
{
    msx::g_host.modelMode = mode;
}

extern "C" int msx_host_get_model_mode(void)
{
    return msx::g_host.modelMode;
}

extern "C" void msx_host_set_view_mode(msx_host_view_mode_t mode)
{
    msx::g_host.viewMode = mode;
}

extern "C" msx_host_view_mode_t msx_host_get_view_mode(void)
{
    return msx::g_host.viewMode;
}

extern "C" const uint8_t* msx_host_get_mapped_rom(const char* fileName, unsigned int* size)
{
    if (size) *size = 0;
    if (!msx::g_host.romData || !msx::g_host.romSize || !fileName) return nullptr;

    const char* base = base_name(fileName);
    const char* myBase = base_name(msx::g_host.romName ? msx::g_host.romName : msx::g_host.romPath);

    if ((msx::g_host.romPath && !strcmp(msx::g_host.romPath, fileName)) ||
        (msx::g_host.romName && base && !strcmp(msx::g_host.romName, base)) ||
        (myBase && base && !strcmp(myBase, base)) ||
        !strcasecmp(base, "CARTA.ROM")) {
        if (size) *size = msx::g_host.romSize;
        return msx::g_host.romData;
    }

    return nullptr;
}

extern "C" const uint8_t* msx_host_get_builtin_file(const char* name, unsigned int* size)
{
    if (size) *size = 0;
    if (!name) return nullptr;

    for (int i = 0; i < msx::g_host.biosCount; ++i) {
        const msx::BiosBlob& blob = msx::g_host.bios[i];
        if (!strcasecmp(blob.name, name)) {
            if (size) *size = blob.size;
            return blob.data;
        }
    }

    return nullptr;
}

extern "C" void msx_host_unload_bios(void)
{
    free_bios();
}

extern "C" int msx_host_load_bios_for_mode(int mode)
{
    free_bios();

    switch (mode & MSX_MODEL) {
        case MSX_MSX2P:
            printf("[MSX] MSX2+ BIOS disabled\n");
            return 0;
        case MSX_MSX2:
            printf("[MSX] MSX2 BIOS disabled\n");
            return 0;
        case MSX_MSX1:
        default:
            return ensure_bios("MSX.ROM") ? 1 : 0;
    }
}

