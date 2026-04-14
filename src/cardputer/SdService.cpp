#include "SdService.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

#include <esp_heap_caps.h>

namespace {

constexpr size_t kDefaultDirectoryLimit = 1024;
constexpr size_t kDefaultRomBrowserLimit = 1024;
constexpr const char* kDirectoryIndexFileName = ".cardputer.idx";
constexpr const char* kDirectoryIndexHeader = "CARDPUTER_ROM_IDX_V1";
constexpr size_t kInitialScanReserve = 48;

std::string getDirectoryIndexPath(const std::string& dirPath) {
    if (dirPath.empty() || dirPath == "/") {
        return std::string("/") + kDirectoryIndexFileName;
    }
    if (dirPath.back() == '/') {
        return dirPath + kDirectoryIndexFileName;
    }
    return dirPath + "/" + kDirectoryIndexFileName;
}

bool hasAllowedExtension(const char* path, size_t len, const std::vector<std::string>& allowedExts) {
    if (allowedExts.empty()) {
        return true;
    }

    size_t dotPos = len;
    for (size_t i = len; i > 0; --i) {
        const char ch = path[i - 1];
        if (ch == '.') {
            dotPos = i - 1;
            break;
        }
        if (ch == '/') {
            break;
        }
    }

    if (dotPos >= len) {
        return false;
    }

    const size_t extLen = len - dotPos;
    for (const std::string& ext : allowedExts) {
        if (extLen != ext.size()) {
            continue;
        }

        bool matches = true;
        for (size_t i = 0; i < extLen; ++i) {
            const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(path[dotPos + i])));
            const char rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
            if (lhs != rhs) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return true;
        }
    }

    return false;
}

std::string trimIndexLine(const std::string& value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }

    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }

    return value.substr(first, last - first);
}

bool trimIndexLineBuffer(char* buffer, size_t& length) {
    if (!buffer) {
        length = 0;
        return false;
    }

    size_t first = 0;
    while (first < length &&
           std::isspace(static_cast<unsigned char>(buffer[first])) != 0) {
        ++first;
    }

    size_t last = length;
    while (last > first &&
           std::isspace(static_cast<unsigned char>(buffer[last - 1])) != 0) {
        --last;
    }

    length = last - first;
    if (length == 0) {
        buffer[0] = '\0';
        return false;
    }

    if (first > 0) {
        std::memmove(buffer, buffer + first, length);
    }
    buffer[length] = '\0';
    return true;
}

std::vector<std::string> scanDirectoryNames(
    const std::string& dirPath,
    size_t limit,
    const std::vector<std::string>* allowedExts,
    bool wantDirectories
) {
    std::vector<std::string> names;

    File dir = SD.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        return names;
    }

    dir.setBufferSize(1024);
    dir.rewindDirectory();
    names.reserve(kInitialScanReserve);

    while (true) {
        bool isDir = false;
        String sname = dir.getNextFileName(&isDir);
        if (!sname.length()) {
            break;
        }

        if (isDir != wantDirectories) {
            continue;
        }

        const char* p = sname.c_str();
        size_t len = sname.length();
        if (len && p[len - 1] == '/') {
            --len;
        }

        size_t start = 0;
        for (size_t k = len; k > 0; --k) {
            if (p[k - 1] == '/') {
                start = k;
                break;
            }
        }

        const size_t namelen = (len > start) ? (len - start) : 0;
        if (namelen == 0) {
            continue;
        }

        if (p[start] == '.') {
            continue;
        }

        if (!isDir && allowedExts != nullptr &&
            !hasAllowedExtension(p + start, namelen, *allowedExts)) {
            continue;
        }

        names.emplace_back(p + start, namelen);
        if (limit > 0 && names.size() >= limit) {
            break;
        }
    }

    dir.close();
    std::sort(names.begin(), names.end());
    return names;
}

size_t writeDirectoryIndexPass(File& indexFile,
                               const std::string& dirPath,
                               size_t limit,
                               const std::vector<std::string>* allowedExts,
                               bool wantDirectories) {
    File dir = SD.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        return 0;
    }

    dir.setBufferSize(1024);
    dir.rewindDirectory();

    size_t written = 0;
    while (true) {
        bool isDir = false;
        String sname = dir.getNextFileName(&isDir);
        if (!sname.length()) {
            break;
        }

        if (isDir != wantDirectories) {
            continue;
        }

        const char* p = sname.c_str();
        size_t len = sname.length();
        if (len && p[len - 1] == '/') {
            --len;
        }

        size_t start = 0;
        for (size_t k = len; k > 0; --k) {
            if (p[k - 1] == '/') {
                start = k;
                break;
            }
        }

        const size_t namelen = (len > start) ? (len - start) : 0;
        if (namelen == 0 || p[start] == '.') {
            continue;
        }

        if (!isDir && allowedExts != nullptr &&
            !hasAllowedExtension(p + start, namelen, *allowedExts)) {
            continue;
        }

        indexFile.write(reinterpret_cast<const uint8_t*>(p + start), namelen);
        indexFile.write('\n');
        ++written;

        if (limit > 0 && written >= limit) {
            break;
        }
    }

    dir.close();
    return written;
}

bool writeDirectoryIndexFile(const std::string& indexPath,
                             const std::string& dirPath,
                             const std::vector<std::string>* allowedExts,
                             size_t limit) {
    if (SD.exists(indexPath.c_str())) {
        (void)SD.remove(indexPath.c_str());
    }

    File indexFile = SD.open(indexPath.c_str(), FILE_WRITE);
    if (!indexFile || indexFile.isDirectory()) {
        if (indexFile) {
            indexFile.close();
        }
        return false;
    }

    indexFile.print(kDirectoryIndexHeader);
    indexFile.write('\n');

    const size_t folderCount = writeDirectoryIndexPass(indexFile, dirPath, limit, allowedExts, true);

    if (limit == 0 || folderCount < limit) {
        const size_t remaining = (limit == 0) ? 0 : (limit - folderCount);
        (void)writeDirectoryIndexPass(indexFile, dirPath, remaining, allowedExts, false);
    }

    indexFile.close();
    return true;
}

} // namespace

SdService::SdService() {}

bool SdService::begin() {
    printf("[SD] Initializing SD card...\n");
    SD.end();
    sdCardSPI.end();
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    delay(5);

    sdCardSPI.begin(
        SD_SCK,
        SD_MISO,
        SD_MOSI,
        SD_CS
    );
    delay(10);

    // Try fast first, then progressively slower for picky cards.
    const uint32_t speeds[] = { 40000000u, 20000000u, 10000000u, 4000000u, 1000000u };
    for (uint32_t hz : speeds) {
        SD.end();
        delay(5);
        printf("[SD] Trying %lu Hz... ", hz);
        if (SD.begin(SD_CS, sdCardSPI, hz, "/sd")) {
            File root = SD.open("/");
            if (root && root.isDirectory()) {
                root.close();
                sdCardMounted = true;
                printf("OK (mounted at /sd)\n");
                return true;
            }

            if (root) {
                root.close();
            }
            printf("FAIL (root not dir)\n");
        } else {
            printf("FAIL\n");
        }
    }

    sdCardMounted = false;
    SD.end();
    printf("[SD] Mount failed at all speeds\n");
    return false;
}

void SdService::close() {
    SD.end();
    sdCardMounted = false;
}

bool SdService::isFile(const std::string& filePath) {
    File f = SD.open(filePath.c_str());
    if (f && !f.isDirectory()) {
        f.close();
        return true;
    }
    return false;
}

bool SdService::isDirectory(const std::string& path) {
    File f = SD.open(path.c_str());
    if (f && f.isDirectory()) {
        f.close();
        return true;
    }
    return false;
}

bool SdService::getFileSize(const std::string& filePath, size_t& outSize) {
    outSize = 0;
    if (!sdCardMounted) {
        return false;
    }

    File file = SD.open(filePath.c_str(), FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        return false;
    }

    outSize = static_cast<size_t>(file.size());
    file.close();
    return true;
}

bool SdService::getSdState() {
    return sdCardMounted;
}

std::vector<std::string> SdService::listElements(
    const std::string& dirPath,
    size_t limit,
    const std::vector<std::string>* allowedExts
) {
    if (limit == 0) {
        limit = (allowedExts == nullptr) ? kDefaultDirectoryLimit : kDefaultRomBrowserLimit;
    }

    std::vector<std::string> elements;
    if (!sdCardMounted) {
        return elements;
    }

    std::vector<std::string> folders = scanDirectoryNames(dirPath, limit, allowedExts, true);
    elements = std::move(folders);

    if (limit == 0 || elements.size() < limit) {
        const size_t remaining = (limit == 0) ? 0 : (limit - elements.size());
        std::vector<std::string> files = scanDirectoryNames(dirPath, remaining, allowedExts, false);
        elements.insert(elements.end(),
                        std::make_move_iterator(files.begin()),
                        std::make_move_iterator(files.end()));
    }

    return elements;
}

std::vector<uint8_t> SdService::readBinaryFile(const std::string& filePath) {
    std::vector<uint8_t> content;
    if (!sdCardMounted) {
        return content;
    }

    File file = SD.open(filePath.c_str(), FILE_READ);
    if (file) {
        content.reserve(file.size());
        while (file.available()) {
            content.push_back(file.read());
        }
        file.close();
    }
    return content;
}

std::string SdService::readFile(const std::string& filePath) {
    std::string content;
    if (!sdCardMounted) {
        return content;
    }

    File file = SD.open(filePath.c_str());
    if (file) {
        while (file.available()) {
            content += static_cast<char>(file.read());
        }
        file.close();
    }
    return content;
}

bool SdService::writeFile(const std::string& filePath, const std::string& data) {
    if (!sdCardMounted) {
        return false;
    }

    if (SD.exists(filePath.c_str()) && !SD.remove(filePath.c_str())) {
        return false;
    }

    File file = SD.open(filePath.c_str(), FILE_WRITE);
    if (file) {
        file.write(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
        file.close();
        return true;
    }
    return false;
}

bool SdService::writeBinaryFile(const std::string& filePath, const std::vector<uint8_t>& data) {
    if (!sdCardMounted) {
        return false;
    }

    File file = SD.open(filePath.c_str(), FILE_WRITE);
    if (file) {
        file.write(data.data(), data.size());
        file.close();
        return true;
    }
    return false;
}

bool SdService::appendToFile(const std::string& filePath, const std::string& data) {
    if (!sdCardMounted) {
        return false;
    }

    File file = SD.open(filePath.c_str(), FILE_APPEND);
    if (file) {
        file.write(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
        file.close();
        return true;
    }
    return false;
}

bool SdService::deleteFile(const std::string& filePath) {
    if (!sdCardMounted) {
        return false;
    }

    if (SD.exists(filePath.c_str())) {
        return SD.remove(filePath.c_str());
    }
    return false;
}

std::string SdService::getFileExt(const std::string& path) {
    size_t pos = path.find_last_of('.');
    return (pos != std::string::npos && pos < path.length() - 1) ? path.substr(pos + 1) : "";
}

std::string SdService::getParentDirectory(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return (pos != std::string::npos && pos > 0) ? path.substr(0, pos) : "/";
}

std::vector<std::string> SdService::getCachedDirectoryElements(
    const std::string& path,
    const std::vector<std::string>* allowedExts,
    size_t limit,
    bool forceRefresh
) {
    if (allowedExts != nullptr) {
        const std::string indexPath = getDirectoryIndexPath(path);

        if (forceRefresh) {
            (void)writeDirectoryIndexFile(indexPath, path, allowedExts, limit);
            return {};
        }

        if (isFile(indexPath)) {
            File indexFile = SD.open(indexPath.c_str(), FILE_READ);
            if (indexFile && !indexFile.isDirectory()) {
                std::vector<std::string> indexedElements;
                bool headerSeen = false;
                char lineBuffer[256];
                bool truncatedByMemory = false;

                while (indexFile.available()) {
                    size_t lineLen = indexFile.readBytesUntil('\n', lineBuffer, sizeof(lineBuffer) - 1);
                    lineBuffer[lineLen] = '\0';
                    if (!trimIndexLineBuffer(lineBuffer, lineLen)) {
                        continue;
                    }
                    if (!headerSeen) {
                        if (std::strcmp(lineBuffer, kDirectoryIndexHeader) != 0) {
                            indexedElements.clear();
                            break;
                        }
                        headerSeen = true;
                        continue;
                    }
                    if (lineBuffer[0] == '.') {
                        continue;
                    }

                    size_t requiredLargestBlock = 1024u;
                    if (indexedElements.size() >= indexedElements.capacity()) {
                        size_t nextCapacity = indexedElements.capacity() == 0 ? 1u : indexedElements.capacity() * 2u;
                        requiredLargestBlock += nextCapacity * sizeof(std::string);
                    }
                    if (lineLen > 15u) {
                        requiredLargestBlock += lineLen + 1u;
                    }

                    const size_t largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
                    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                    if (largestFreeBlock < requiredLargestBlock || freeHeap < (requiredLargestBlock + 4096u)) {
                        truncatedByMemory = true;
                        break;
                    }

                    indexedElements.emplace_back(lineBuffer, lineLen);
                    if (limit > 0 && indexedElements.size() >= limit) {
                        break;
                    }
                }

                indexFile.close();
                if (truncatedByMemory) {
                    std::printf("[SD] directory index truncated by heap pressure: path=%s count=%u free8=%u largest8=%u\n",
                                indexPath.c_str(),
                                static_cast<unsigned>(indexedElements.size()),
                                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
                }
                if (headerSeen) {
                    return indexedElements;
                }
            } else if (indexFile) {
                indexFile.close();
            }
        }

        if (!writeDirectoryIndexFile(indexPath, path, allowedExts, limit)) {
            return listElements(path, limit, allowedExts);
        }

        return getCachedDirectoryElements(path, allowedExts, limit, false);
    }

    return listElements(path, limit, allowedExts);
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

    if (!SD.exists(directory.c_str())) {
        return SD.mkdir(directory.c_str()); // Create forlder
    }
    return true; // Folder already exists
}
