#include "compat/preferences_compat.h"

#include <vector>

#include "esp_err.h"
#include "nvs_flash.h"

static esp_err_t ensure_nvs_ready()
{
    static bool initialized = false;
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        initialized = true;
    }
    return err;
}

Preferences::~Preferences()
{
    end();
}

bool Preferences::begin(const char* name, bool readOnly)
{
    end();
    if (!name || ensure_nvs_ready() != ESP_OK) {
        return false;
    }

    readOnly_ = readOnly;
    const nvs_open_mode_t mode = readOnly ? NVS_READONLY : NVS_READWRITE;
    return nvs_open(name, mode, &handle_) == ESP_OK;
}

void Preferences::end()
{
    if (handle_) {
        nvs_close(handle_);
        handle_ = 0;
    }
}

bool Preferences::isKey(const char* key) const
{
    if (!handle_ || !key) {
        return false;
    }

    size_t len = 0;
    esp_err_t err = nvs_get_str(handle_, key, nullptr, &len);
    if (err == ESP_OK) {
        return true;
    }

    uint8_t value = 0;
    return nvs_get_u8(handle_, key, &value) == ESP_OK;
}

bool Preferences::getBool(const char* key, bool defaultValue) const
{
    uint8_t value = defaultValue ? 1 : 0;
    if (!handle_ || !key || nvs_get_u8(handle_, key, &value) != ESP_OK) {
        return defaultValue;
    }
    return value != 0;
}

size_t Preferences::putBool(const char* key, bool value)
{
    if (!handle_ || readOnly_ || !key) {
        return 0;
    }
    if (nvs_set_u8(handle_, key, value ? 1 : 0) != ESP_OK) {
        return 0;
    }
    return nvs_commit(handle_) == ESP_OK ? 1 : 0;
}

uint8_t Preferences::getUChar(const char* key, uint8_t defaultValue) const
{
    uint8_t value = defaultValue;
    if (!handle_ || !key || nvs_get_u8(handle_, key, &value) != ESP_OK) {
        return defaultValue;
    }
    return value;
}

size_t Preferences::putUChar(const char* key, uint8_t value)
{
    if (!handle_ || readOnly_ || !key) {
        return 0;
    }
    if (nvs_set_u8(handle_, key, value) != ESP_OK) {
        return 0;
    }
    return nvs_commit(handle_) == ESP_OK ? 1 : 0;
}

std::string Preferences::getString(const char* key, const char* defaultValue) const
{
    if (!handle_ || !key) {
        return defaultValue ? defaultValue : "";
    }

    size_t len = 0;
    if (nvs_get_str(handle_, key, nullptr, &len) != ESP_OK || len == 0) {
        return defaultValue ? defaultValue : "";
    }

    std::vector<char> buffer(len);
    if (nvs_get_str(handle_, key, buffer.data(), &len) != ESP_OK) {
        return defaultValue ? defaultValue : "";
    }

    return std::string(buffer.data());
}

size_t Preferences::putString(const char* key, const char* value)
{
    if (!handle_ || readOnly_ || !key || !value) {
        return 0;
    }
    if (nvs_set_str(handle_, key, value) != ESP_OK) {
        return 0;
    }
    return nvs_commit(handle_) == ESP_OK ? std::char_traits<char>::length(value) : 0;
}
