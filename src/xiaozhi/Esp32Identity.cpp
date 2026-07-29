#include "Esp32Identity.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_system.h>

#include <array>
#include <cstdio>

namespace xiaozhi {

std::string Esp32Identity::deviceId() {
    std::array<uint8_t, 6> mac{};
    if (esp_read_mac(mac.data(), ESP_MAC_WIFI_STA) != ESP_OK) {
        return {};
    }
    char output[18] = {};
    std::snprintf(output, sizeof(output), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                  mac[2], mac[3], mac[4], mac[5]);
    return output;
}

std::string Esp32Identity::persistentClientId() {
    Preferences preferences;
    if (!preferences.begin("board", false)) {
        return generateUuid();
    }
    String stored = preferences.getString("uuid", "");
    if (stored.isEmpty()) {
        const std::string generated = generateUuid();
        if (preferences.putString("uuid", generated.c_str()) != generated.size()) {
            preferences.end();
            return generated;
        }
        stored = generated.c_str();
    }
    preferences.end();
    return std::string(stored.c_str());
}

std::string Esp32Identity::firmwareVersion() {
    const esp_app_desc_t* description = esp_app_get_description();
    return description == nullptr ? std::string() : std::string(description->version);
}

std::string Esp32Identity::generateUuid() {
    std::array<uint8_t, 16> bytes{};
    esp_fill_random(bytes.data(), bytes.size());
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    char output[37] = {};
    std::snprintf(output, sizeof(output),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                  bytes[15]);
    return output;
}

}  // namespace xiaozhi
