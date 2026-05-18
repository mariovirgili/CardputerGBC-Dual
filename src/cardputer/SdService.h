#pragma GCC optimize ("Os")

#ifndef SD_SERVICE_H
#define SD_SERVICE_H

#include <vector>
#include <string>
#include <unordered_map>

#include "sdmmc_cmd.h"

static constexpr int SD_SCK  = 40;
static constexpr int SD_MISO = 39;
static constexpr int SD_MOSI = 14;
static constexpr int SD_CS   = 12;

class SdService {
private:
    bool sdCardMounted = false;
    sdmmc_card_t* card = nullptr;
    std::unordered_map<std::string, std::vector<std::string>> cachedDirectoryElements;

    std::string toMountedPath(const std::string& path) const;
public:
    SdService();

    bool begin();
    void close();
    bool isFile(const std::string& filePath);
    bool isDirectory(const std::string& path);
    bool getSdState();

    std::vector<std::string> listElements(const std::string& dirPath, size_t limit = 0);
    std::vector<uint8_t> readBinaryFile(const std::string& filePath);
    std::string readFile(const std::string& filePath);

    bool writeFile(const std::string& filePath, const std::string& data);
    bool writeBinaryFile(const std::string& filePath, const std::vector<uint8_t>& data);
    bool appendToFile(const std::string& filePath, const std::string& data);
    bool deleteFile(const std::string& filePath);
    bool ensureDirectory(const std::string& directory);
    bool writeDirectoryIndex(const std::string& dirPath, size_t* outCount = nullptr);

    std::string getFileExt(const std::string& path);
    std::string getParentDirectory(const std::string& path);
    std::string getFileName(const std::string& path);
    std::vector<std::string> getCachedDirectoryElements(const std::string& path);
    void setCachedDirectoryElements(const std::string& path, const std::vector<std::string>& elements);
    void removeCachedPath(const std::string& path);
};

#endif // SD_SERVICE_H
