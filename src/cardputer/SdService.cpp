#pragma GCC optimize ("Os")

#include "SdService.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compat/arduino_compat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"

SdService::SdService() {}

namespace {

static void trimLineEnd(std::string& line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
}

static bool readDirectoryIndex(const std::string& mountedDir, size_t limit, std::vector<std::string>& out) {
    const std::string indexPath = mountedDir + "/.idx";
    FILE* index = fopen(indexPath.c_str(), "rb");
    if (!index) return false;

    char lineBuf[256];
    while (out.size() < limit && fgets(lineBuf, sizeof(lineBuf), index)) {
        std::string line(lineBuf);
        trimLineEnd(line);
        if (line.empty() || line[0] == '.' || line[0] == '#') continue;
        out.emplace_back(line);
    }
    fclose(index);
    return true;
}

}

std::string SdService::toMountedPath(const std::string& path) const {
    if (path.rfind("/sd", 0) == 0) {
        return path;
    }
    if (path.empty() || path[0] != '/') {
        return "/sd/" + path;
    }
    return "/sd" + path;
}

bool SdService::begin() {
    if (sdCardMounted) {
        return true;
    }

    delay(10);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_MOSI;
    bus_cfg.miso_io_num = SD_MISO;
    bus_cfg.sclk_io_num = SD_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 8 * 1024;

    esp_err_t err = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        sdCardMounted = false;
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)SD_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 4 * 1024;

    err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot_config, &mount_config, &card);
    if (err == ESP_OK) {
        sdCardMounted = true;
        return true;
    }

    sdCardMounted = false;
    spi_bus_free((spi_host_device_t)host.slot);
    return false;
}

void SdService::close() {
    if (sdCardMounted) {
        esp_vfs_fat_sdcard_unmount("/sd", card);
        spi_bus_free(SPI2_HOST);
    }
    card = nullptr;
    sdCardMounted = false;
}

bool SdService::isFile(const std::string& filePath) {
    struct stat st = {};
    const std::string mounted = toMountedPath(filePath);
    return stat(mounted.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool SdService::isDirectory(const std::string& path) {
    struct stat st = {};
    const std::string mounted = toMountedPath(path);
    return stat(mounted.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool SdService::getSdState() {
    return sdCardMounted;
}

std::vector<std::string> SdService::listElements(const std::string& dirPath, size_t limit) {
    if (limit == 0) limit = 2048;

    std::vector<std::string> filesList;
    std::vector<std::string> foldersList;
    filesList.reserve(96);
    foldersList.reserve(32);

    if (!sdCardMounted) return filesList;

    const std::string mountedDir = toMountedPath(dirPath);
    if (readDirectoryIndex(mountedDir, limit, filesList)) {
        return filesList;
    }

    if (writeDirectoryIndex(dirPath, nullptr) && readDirectoryIndex(mountedDir, limit, filesList)) {
        return filesList;
    }

    DIR* dir = opendir(mountedDir.c_str());
    if (!dir) return filesList;

    size_t i = 0;
    while (true) {
        dirent* entry = readdir(dir);
        if (!entry) break;

        const char* name = entry->d_name;
        if (!name || name[0] == '\0' || name[0] == '.') continue;

        const std::string fullPath = mountedDir + "/" + name;
        struct stat st = {};
        const bool isDir = (stat(fullPath.c_str(), &st) == 0) && S_ISDIR(st.st_mode);

        // push to the right list
        if (isDir) foldersList.emplace_back(name);
        else       filesList.emplace_back(name);

        if (++i >= limit) break;
    }

    closedir(dir);

    std::sort(foldersList.begin(), foldersList.end());
    std::sort(filesList.begin(), filesList.end());
    foldersList.insert(foldersList.end(), filesList.begin(), filesList.end());
    return foldersList;
}

bool SdService::writeDirectoryIndex(const std::string& dirPath, size_t* outCount) {
    if (outCount) *outCount = 0;
    if (!sdCardMounted) return false;

    const std::string mountedDir = toMountedPath(dirPath);
    DIR* dir = opendir(mountedDir.c_str());
    if (!dir) return false;

    std::vector<std::string> filesList;
    std::vector<std::string> foldersList;
    filesList.reserve(96);
    foldersList.reserve(32);

    while (true) {
        dirent* entry = readdir(dir);
        if (!entry) break;

        const char* name = entry->d_name;
        if (!name || name[0] == '\0' || name[0] == '.') continue;

        const std::string fullPath = mountedDir + "/" + name;
        struct stat st = {};
        const bool isDir = (stat(fullPath.c_str(), &st) == 0) && S_ISDIR(st.st_mode);

        if (isDir) foldersList.emplace_back(name);
        else       filesList.emplace_back(name);
    }

    closedir(dir);

    std::sort(foldersList.begin(), foldersList.end());
    std::sort(filesList.begin(), filesList.end());

    const std::string tempPath = mountedDir + "/.idx.tmp";
    const std::string indexPath = mountedDir + "/.idx";
    FILE* index = fopen(tempPath.c_str(), "wb");
    if (!index) return false;

    size_t count = 0;
    for (const auto& name : foldersList) {
        if (fprintf(index, "%s\n", name.c_str()) < 0) {
            fclose(index);
            unlink(tempPath.c_str());
            return false;
        }
        ++count;
    }
    for (const auto& name : filesList) {
        if (fprintf(index, "%s\n", name.c_str()) < 0) {
            fclose(index);
            unlink(tempPath.c_str());
            return false;
        }
        ++count;
    }

    if (fclose(index) != 0) {
        unlink(tempPath.c_str());
        return false;
    }

    unlink(indexPath.c_str());
    if (rename(tempPath.c_str(), indexPath.c_str()) != 0) {
        unlink(tempPath.c_str());
        return false;
    }

    if (outCount) *outCount = count;
    return true;
}

std::vector<uint8_t> SdService::readBinaryFile(const std::string& filePath) {
    std::vector<uint8_t> content;
    if (!sdCardMounted) {
        return content;
    }

    const std::string mounted = toMountedPath(filePath);
    FILE* file = fopen(mounted.c_str(), "rb");
    if (file) {
        if (fseek(file, 0, SEEK_END) == 0) {
            long size = ftell(file);
            if (size > 0) {
                content.resize((size_t)size);
                rewind(file);
                const size_t got = fread(content.data(), 1, content.size(), file);
                content.resize(got);
            }
        }
        fclose(file);
    }
    return content;
}

std::string SdService::readFile(const std::string& filePath) {
    std::string content;
    if (!sdCardMounted) {
        return content;
    }

    const std::string mounted = toMountedPath(filePath);
    FILE* file = fopen(mounted.c_str(), "rb");
    if (file) {
        char buffer[512];
        size_t got = 0;
        while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
            content.append(buffer, got);
        }
        fclose(file);
    }
    return content;
}

bool SdService::writeFile(const std::string& filePath, const std::string& data) {
    if (!sdCardMounted) {
        return false;
    }

    const std::string mounted = toMountedPath(filePath);
    FILE* file = fopen(mounted.c_str(), "wb");
    if (!file) return false;
    const size_t written = fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    return written == data.size();
}

bool SdService::writeBinaryFile(const std::string& filePath, const std::vector<uint8_t>& data) {
    if (!sdCardMounted) {
        return false;
    }

    const std::string mounted = toMountedPath(filePath);
    FILE* file = fopen(mounted.c_str(), "wb");
    if (!file) return false;
    const size_t written = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    return written == data.size();
}

bool SdService::appendToFile(const std::string& filePath, const std::string& data) {
    if (!sdCardMounted) {
        return false;
    }

    const std::string mounted = toMountedPath(filePath);
    FILE* file = fopen(mounted.c_str(), "ab");
    if (!file) return false;
    const size_t written = fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    return written == data.size();
}

bool SdService::deleteFile(const std::string& filePath) {
    if (!sdCardMounted) {
        return false;
    }

    const std::string mounted = toMountedPath(filePath);
    return unlink(mounted.c_str()) == 0;
}

std::string SdService::getFileExt(const std::string& path) {
    size_t pos = path.find_last_of('.');
    return (pos != std::string::npos && pos < path.length() - 1) ? path.substr(pos + 1) : "";
}

std::string SdService::getParentDirectory(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return (pos != std::string::npos && pos > 0) ? path.substr(0, pos) : "/";
}

std::vector<std::string> SdService::getCachedDirectoryElements(const std::string& path) {
    // if (cachedDirectoryElements.find(path) != cachedDirectoryElements.end()) {
    //     return cachedDirectoryElements[path];
    // }

    // std::vector<std::string> elements = listElements(path);
    // if (elements.size() > 4) {
    //     if (cachedDirectoryElements.size() >= 64) {
    //         cachedDirectoryElements.erase(cachedDirectoryElements.begin());
    //     }
    //     cachedDirectoryElements[path] = elements;
    // }
    // return elements;
    return listElements(path); // We dont rly need caching anymore
}

void SdService::setCachedDirectoryElements(const std::string& path, const std::vector<std::string>& elements) {
    // cachedDirectoryElements[path] = elements; // We dont rly need this anymore
}

void SdService::removeCachedPath(const std::string& path) {
    // cachedDirectoryElements.erase(path); // We dont rly need this anymore
}

std::string SdService::getFileName(const std::string& path) {
    size_t lastSlash = path.find_last_of('/');
    size_t lastDot = path.find_last_of('.');
    if (lastDot == std::string::npos || lastDot < lastSlash) {
        lastDot = path.length(); // Not ext, get end of path
    }
    size_t start = (lastSlash != std::string::npos) ? lastSlash + 1 : 0;
    return path.substr(start, lastDot - start);
}

bool SdService::ensureDirectory(const std::string& directory) {
    if (!sdCardMounted) {
        return false;
    }

    const std::string mounted = toMountedPath(directory);
    struct stat st = {};
    if (stat(mounted.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(mounted.c_str(), 0775) == 0;
}
