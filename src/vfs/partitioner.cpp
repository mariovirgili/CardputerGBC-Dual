#pragma GCC optimize ("Os")

#include <string.h>
#include <stdint.h>
#include "compat/arduino_compat.h"

#include "partitioner.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp32s3/rom/spi_flash.h"
#include "esp_partition.h"
#include "esp_ipc_isr.h"

// Sector size for partition table
#define PARTITION_SIZE 4096
static const uint32_t PARTITION_ADDR = 0x00008000;
static const uint32_t PARTITION_SECTOR = PARTITION_ADDR / 0x1000;
static const size_t LAUNCHER_SPIFFS_SIZE = 1 * 1024 * 1024; // 1 MB
static const int MAX_WRITE_RETRIES = 3;

static portMUX_TYPE s_partitionMux = portMUX_INITIALIZER_UNLOCKED;

// Gamestation with 4MB of SPIFFS for the Launcher
const uint8_t gamestation[192] PROGMEM = {
    0xAA, 0x50, 0x01, 0x02, 0x00, 0x90, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x6E, 0x76, 0x73, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAA, 0x50, 0x00, 0x20, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x16, 0x00, 0x61, 0x70, 0x70, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAA, 0x50, 0x00, 0x10, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x28, 0x00, 0x61, 0x70, 0x70, 0x31,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAA, 0x50, 0x01, 0x82, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x40, 0x00, 0x73, 0x70, 0x69, 0x66,
    0x66, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAA, 0x50, 0x01, 0x03, 0x00, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x01, 0x00, 0x63, 0x6F, 0x72, 0x65,
    0x64, 0x75, 0x6D, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xEB, 0xEB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xEA, 0x8A, 0x51, 0x4E, 0xB6, 0x29, 0x5B, 0x5A, 0xAC, 0xC4, 0x21, 0xE7, 0xC7, 0x83, 0xDB, 0x6A
};

static bool build_partition_buffer(uint8_t *buf8) {
    if (!buf8) {
        return false;
    }

    memset(buf8, 0xFF, PARTITION_SIZE);
    memcpy(buf8, gamestation, sizeof(gamestation));
    return true;
}

static bool verify_partition_buffer(const uint8_t *expectedBuf, uint8_t *readBuf) {
    if (!expectedBuf || !readBuf) {
        return false;
    }

    esp_err_t err = esp_flash_read(NULL, readBuf, PARTITION_ADDR, PARTITION_SIZE);
    if (err != ESP_OK) {
        return false;
    }

    for (size_t i = 0; i < PARTITION_SIZE; ++i) {
        if (readBuf[i] != expectedBuf[i]) {
            return false;
        }
    }

    return true;
}

static bool write_gamestation_partition_once(const uint32_t *buf32) {
    if (!buf32) {
        return false;
    }

    int eraseRc = -1;
    int writeRc = -1;

    esp_ipc_isr_stall_other_cpu();

    portENTER_CRITICAL(&s_partitionMux);

    eraseRc = esp_rom_spiflash_erase_sector(PARTITION_SECTOR);
    if (eraseRc == 0) {
        writeRc = esp_rom_spiflash_write(PARTITION_ADDR, buf32, PARTITION_SIZE);
    }

    portEXIT_CRITICAL(&s_partitionMux);

    esp_ipc_isr_release_other_cpu();

    return (eraseRc == 0 && writeRc == 0);
}

static bool write_gamestation_partition() {
    EMU_LOG("[ROM] Preparing Game Station partition buffer...\n");

    uint8_t *buf8 = (uint8_t *)heap_caps_malloc(
        PARTITION_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
    if (!buf8) {
        EMU_LOG("[ROM][ERROR] Failed to allocate partition buffer\n");
        return false;
    }

    uint8_t *readBuf = (uint8_t *)heap_caps_malloc(
        PARTITION_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
    if (!readBuf) {
        EMU_LOG("[ROM][ERROR] Failed to allocate verify buffer\n");
        heap_caps_free(buf8);
        return false;
    }

    if ((((uintptr_t)buf8) & 0x3) != 0) {
        EMU_LOG("[ROM][ERROR] Partition buffer is not 4-byte aligned\n");
        heap_caps_free(readBuf);
        heap_caps_free(buf8);
        return false;
    }

    if (!build_partition_buffer(buf8)) {
        EMU_LOG("[ROM][ERROR] Failed to build partition buffer\n");
        heap_caps_free(readBuf);
        heap_caps_free(buf8);
        return false;
    }

    const uint32_t *buf32 = reinterpret_cast<const uint32_t *>(buf8);

    for (int attempt = 1; attempt <= MAX_WRITE_RETRIES; ++attempt) {
        EMU_LOG("[ROM] Write attempt %d/%d...\n", attempt, MAX_WRITE_RETRIES);

        if (!write_gamestation_partition_once(buf32)) {
            delay(1);
            yield();
            continue;
        }

        if (verify_partition_buffer(buf8, readBuf)) {
            EMU_LOG("[ROM] Full sector verification OK.\n");
            heap_caps_free(readBuf);
            heap_caps_free(buf8);
            return true;
        }

        delay(1);
        yield();
    }

    heap_caps_free(readBuf);
    heap_caps_free(buf8);
    EMU_LOG("[ROM][ERROR] All write attempts failed.\n");
    return false;
}

// Detect if we are running in Launcher (1 MB SPIFFS)
bool isLauncherLayout() {
    const esp_partition_t *spiffs =
        esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
            "spiffs"
        );

    if (!spiffs) {
        EMU_LOG("[GUARD] No SPIFFS partition with label 'spiffs' found, aborting.\n");
        return false;
    }

    EMU_LOG("[GUARD] Current SPIFFS size: 0x%X (%u KB)\n",
            (unsigned)spiffs->size, (unsigned)(spiffs->size / 1024));

    if (spiffs->size != LAUNCHER_SPIFFS_SIZE) {
        EMU_LOG("[GUARD] SPIFFS size != 1 MiB (launcher layout), aborting.\n");
        return false;
    }

    EMU_LOG("[GUARD] Launcher layout detected (SPIFFS = 1 MiB).\n");
    return true;
}

bool flashGameStationPartition() {
    EMU_LOG("Writing Game Station partition (ROM hack)...\n");

    if (!write_gamestation_partition()) {
        EMU_LOG("[ERROR] Game Station partition write FAILED\n");
        return false;
    }

    EMU_LOG("[OK] Game Station partition verified successfully.\n");
    return true;
}
