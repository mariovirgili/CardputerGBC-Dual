#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "nvs.h"

class Preferences {
public:
    Preferences() = default;
    ~Preferences();

    bool begin(const char* name, bool readOnly = false);
    void end();

    bool isKey(const char* key) const;

    bool getBool(const char* key, bool defaultValue = false) const;
    size_t putBool(const char* key, bool value);

    uint8_t getUChar(const char* key, uint8_t defaultValue = 0) const;
    size_t putUChar(const char* key, uint8_t value);

    std::string getString(const char* key, const char* defaultValue = "") const;
    size_t putString(const char* key, const char* value);

private:
    nvs_handle_t handle_ = 0;
    bool readOnly_ = true;
};
