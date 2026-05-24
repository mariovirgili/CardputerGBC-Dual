#ifndef ROM_FLASH_IO_H
#define ROM_FLASH_IO_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CopyProgressCallback)(size_t total, size_t current, void* user_ctx);

const esp_partition_t* findRomPartition(const char* name);
bool eraseRomPartition(const esp_partition_t* part, size_t bytes_to_write);
bool copyFileToPartition(const char* srcPath, const esp_partition_t* part, size_t* outSize, CopyProgressCallback progressCb, void* progressCtx);
bool copyFileToPartitionByteSwap16(const char* srcPath, const esp_partition_t* part, size_t* outSize, CopyProgressCallback progressCb, void* progressCtx);
#ifdef __cplusplus
}
#endif

#endif /* ROM_FLASH_IO_H */
