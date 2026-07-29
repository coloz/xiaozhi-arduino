#include "Provisioning.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_chip_info.h>
#include <sdkconfig.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <vector>

#include "Esp32Identity.h"

namespace xiaozhi {
namespace {

constexpr size_t kMaximumFieldBytes = 2048;

constexpr char kOfficialRootCa[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBhMQswCQYDVQQG
EwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cuZGlnaWNlcnQuY29tMSAw
HgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUx
MjAwMDBaMGExCzAJBgNVBAYTAlVTMRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3
dy5kaWdpY2VydC5jb20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkq
hkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI2/Ou8jqJ
kTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx1x7e/dfgy5SDN67sH0NO
3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQq2EGnI/yuum06ZIya7XzV+hdG82MHauV
BJVJ8zUtluNJbd134/tJS7SsVQepj5WztCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyM
UNGPHgm+F6HmIcr9g+UQvIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQAB
o0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV5uNu
5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY1Yl9PMWLSn/pvtsr
F9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4NeF22d+mQrvHRAiGfzZ0JFrabA0U
WTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NGFdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBH
QRFXGU7Aj64GxJUTFy8bJZ918rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/
iyK5S9kJRaTepLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----)PEM";

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
extern const uint8_t x509CrtBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509CrtBundleEnd[] asm("_binary_x509_crt_bundle_end");
#endif

bool hasControlCharacter(const std::string& value, bool reject_space = false) {
    return std::any_of(value.begin(), value.end(), [reject_space](unsigned char character) {
        return character < (reject_space ? 0x21U : 0x20U) || character == 0x7fU;
    });
}

bool validHttpUrl(const std::string& value) {
    return !value.empty() && value.size() <= kMaximumFieldBytes &&
           !hasControlCharacter(value, true) &&
           (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0);
}

bool validWebSocketUrl(const std::string& value) {
    return !value.empty() && value.size() <= kMaximumFieldBytes &&
           !hasControlCharacter(value, true) &&
           (value.rfind("ws://", 0) == 0 || value.rfind("wss://", 0) == 0);
}

bool validHeaderValue(const std::string& value, size_t maximum, bool required = true) {
    return (!required || !value.empty()) && value.size() <= maximum &&
           !hasControlCharacter(value);
}

bool readString(JsonObjectConst object, const char* key, std::string& output,
                bool required = false) {
    JsonVariantConst value = object[key];
    if (value.isUnbound()) {
        return !required;
    }
    if (!value.is<const char*>()) {
        return false;
    }
    const JsonString string = value.as<JsonString>();
    const size_t size = string.size();
    if (size > kMaximumFieldBytes) {
        return false;
    }
    output.assign(string.c_str(), size);
    return !required || !output.empty();
}

bool parseVersion(const std::string& input, std::vector<uint32_t>& output) {
    output.clear();
    size_t position = (!input.empty() && (input[0] == 'v' || input[0] == 'V')) ? 1 : 0;
    if (position == input.size()) {
        return false;
    }

    while (position < input.size()) {
        if (input[position] == '-' || input[position] == '+') {
            break;
        }
        if (!std::isdigit(static_cast<unsigned char>(input[position]))) {
            return false;
        }
        uint64_t number = 0;
        while (position < input.size() &&
               std::isdigit(static_cast<unsigned char>(input[position]))) {
            number = number * 10U + static_cast<uint64_t>(input[position] - '0');
            if (number > std::numeric_limits<uint32_t>::max()) {
                return false;
            }
            ++position;
        }
        output.push_back(static_cast<uint32_t>(number));
        if (position == input.size() || input[position] == '-' || input[position] == '+') {
            break;
        }
        if (input[position] != '.') {
            return false;
        }
        ++position;
        if (position == input.size()) {
            return false;
        }
    }
    return !output.empty();
}

class BoundedStringStream final : public Stream {
public:
    explicit BoundedStringStream(size_t limit) : limit_(limit) { value_.reserve(limit); }

    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t* buffer, size_t size) override {
        if (buffer == nullptr || overflowed_ || value_.size() + size > limit_) {
            overflowed_ = true;
            return 0;
        }
        value_.append(reinterpret_cast<const char*>(buffer), size);
        return size;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    bool overflowed() const { return overflowed_; }
    const std::string& value() const { return value_; }

private:
    size_t limit_;
    bool overflowed_ = false;
    std::string value_;
};

bool synchronizeClock(const OfficialServiceOptions& options, std::string& error) {
    constexpr time_t kKnownGoodEpoch = 1700000000;
    if (time(nullptr) >= kKnownGoodEpoch) {
        return true;
    }
    if (options.time_sync_timeout_ms == 0 || options.time_sync_timeout_ms > 600000 ||
        options.primary_ntp_server == nullptr || options.primary_ntp_server[0] == '\0') {
        error = "official service has invalid time synchronization options";
        return false;
    }

    configTime(0, 0, options.primary_ntp_server, options.secondary_ntp_server);
    const uint32_t started = millis();
    while (time(nullptr) < kKnownGoodEpoch &&
           millis() - started < options.time_sync_timeout_ms) {
        delay(100);
    }
    if (time(nullptr) < kKnownGoodEpoch) {
        error = "NTP time synchronization failed";
        return false;
    }
    return true;
}

std::string buildOfficialSystemInfo(const std::string& device_id,
                                    const std::string& client_id,
                                    const OfficialServiceOptions& options) {
    JsonDocument document;
    document["version"] = 2;
    document["language"] = options.language;
    document["flash_size"] = ESP.getFlashChipSize();
    document["psram_size"] = ESP.getPsramSize();
    document["minimum_free_heap_size"] = ESP.getMinFreeHeap();
    document["mac_address"] = device_id;
    document["uuid"] = client_id;
    document["chip_model_name"] = "esp32s3";

    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);
    JsonObject chip = document["chip_info"].to<JsonObject>();
    chip["model"] = static_cast<int>(chip_info.model);
    chip["cores"] = chip_info.cores;
    chip["revision"] = chip_info.revision;
    chip["features"] = chip_info.features;

    JsonObject application = document["application"].to<JsonObject>();
    application["name"] = "xiaozhi-arduino";
    application["version"] = Esp32Identity::firmwareVersion();
    application["compile_time"] = __DATE__ "T" __TIME__ "Z";
    application["idf_version"] = ESP.getSdkVersion();
    application["elf_sha256"] = "";
    document["partition_table"].to<JsonArray>();
    document["ota"]["label"] = "app";
    document["board"]["type"] = options.board_type;
    document["board"]["name"] = options.board_name;

    std::string output;
    serializeJson(document, output);
    return output;
}

}  // namespace

bool Provisioning::parse(const uint8_t* data, size_t size, ProvisioningResult& output,
                         std::string& error, size_t max_json_bytes) {
    output = {};
    if (data == nullptr || size == 0 || size > max_json_bytes) {
        error = "provisioning response is empty or exceeds the configured limit";
        return false;
    }

    JsonDocument document;
    const DeserializationError json_error = deserializeJson(document, data, size);
    if (json_error || !document.is<JsonObject>()) {
        error = json_error ? std::string("invalid provisioning JSON: ") + json_error.c_str()
                           : "provisioning response must be an object";
        return false;
    }
    JsonObjectConst root = document.as<JsonObjectConst>();

    JsonVariantConst websocket_value = root["websocket"];
    const bool has_websocket = !websocket_value.isUnbound();
    if (has_websocket && !websocket_value.is<JsonObjectConst>()) {
        error = "websocket provisioning section must be an object";
        return false;
    }
    if (has_websocket) {
        JsonObjectConst websocket = websocket_value.as<JsonObjectConst>();
        if (!readString(websocket, "url", output.websocket.url, true) ||
            !readString(websocket, "token", output.websocket.token)) {
            error = "invalid websocket provisioning section";
            return false;
        }
        JsonVariantConst version = websocket["version"];
        if (!version.isUnbound()) {
            if (!version.is<uint8_t>()) {
                error = "websocket protocol version must be an integer in 1..3";
                return false;
            }
            output.websocket.version = version.as<uint8_t>();
        }
        if (output.websocket.version < 1 || output.websocket.version > 3 ||
            !validWebSocketUrl(output.websocket.url) ||
            hasControlCharacter(output.websocket.token)) {
            error = "websocket provisioning has an invalid URL or protocol version";
            return false;
        }
        output.websocket.present = true;
    }

    JsonVariantConst mqtt_value = root["mqtt"];
    const bool has_mqtt = !mqtt_value.isUnbound();
    if (has_mqtt && !mqtt_value.is<JsonObjectConst>()) {
        error = "MQTT provisioning section must be an object";
        return false;
    }
    if (has_mqtt) {
        JsonObjectConst mqtt = mqtt_value.as<JsonObjectConst>();
        if (!readString(mqtt, "endpoint", output.mqtt.endpoint, true) ||
            !readString(mqtt, "client_id", output.mqtt.client_id, true) ||
            !readString(mqtt, "username", output.mqtt.username) ||
            !readString(mqtt, "password", output.mqtt.password) ||
            !readString(mqtt, "publish_topic", output.mqtt.publish_topic, true)) {
            error = "invalid MQTT provisioning section";
            return false;
        }
        JsonVariantConst keepalive = mqtt["keepalive"];
        if (!keepalive.isUnbound()) {
            if (!keepalive.is<uint32_t>()) {
                error = "MQTT keepalive must be an integer";
                return false;
            }
            output.mqtt.keepalive_seconds = keepalive.as<uint32_t>();
        }
        if (output.mqtt.keepalive_seconds < 10 || output.mqtt.keepalive_seconds > 86400) {
            error = "MQTT keepalive is outside 10..86400 seconds";
            return false;
        }
        output.mqtt.present = true;
    }

    JsonVariantConst activation_value = root["activation"];
    const bool has_activation = !activation_value.isUnbound();
    if (has_activation && !activation_value.is<JsonObjectConst>()) {
        error = "activation provisioning section must be an object";
        return false;
    }
    if (has_activation) {
        JsonObjectConst activation = activation_value.as<JsonObjectConst>();
        if (!readString(activation, "message", output.activation.message) ||
            !readString(activation, "code", output.activation.code) ||
            !readString(activation, "challenge", output.activation.challenge)) {
            error = "invalid activation provisioning section";
            return false;
        }
        JsonVariantConst timeout = activation["timeout_ms"];
        if (!timeout.isUnbound()) {
            if (!timeout.is<uint32_t>()) {
                error = "activation timeout must be an integer";
                return false;
            }
            output.activation.timeout_ms = timeout.as<uint32_t>();
        }
        if (output.activation.timeout_ms == 0 || output.activation.timeout_ms > 600000) {
            error = "activation timeout is outside 1..600000 ms";
            return false;
        }
        output.activation.present = !output.activation.message.empty() ||
                                    !output.activation.code.empty() ||
                                    !output.activation.challenge.empty();
    }

    JsonVariantConst firmware_value = root["firmware"];
    const bool has_firmware = !firmware_value.isUnbound();
    if (has_firmware && !firmware_value.is<JsonObjectConst>()) {
        error = "firmware provisioning section must be an object";
        return false;
    }
    if (has_firmware) {
        JsonObjectConst firmware = firmware_value.as<JsonObjectConst>();
        if (!readString(firmware, "version", output.firmware.version, true) ||
            !readString(firmware, "url", output.firmware.url)) {
            error = "invalid firmware provisioning section";
            return false;
        }
        if ((!output.firmware.url.empty() && !validHttpUrl(output.firmware.url)) ||
            hasControlCharacter(output.firmware.version)) {
            error = "firmware provisioning has an unsafe URL or version";
            return false;
        }
        JsonVariantConst force_value = firmware["force"];
        if (!force_value.isUnbound()) {
            if (force_value.is<bool>()) {
                output.firmware.force = force_value.as<bool>();
            } else if (force_value.is<int32_t>()) {
                const int32_t force = force_value.as<int32_t>();
                if (force != 0 && force != 1) {
                    error = "firmware force must be a boolean or 0/1";
                    return false;
                }
                output.firmware.force = force == 1;
            } else {
                error = "firmware force must be a boolean or 0/1";
                return false;
            }
        }
        output.firmware.present = true;
    }

    JsonVariantConst server_time_value = root["server_time"];
    const bool has_server_time = !server_time_value.isUnbound();
    if (has_server_time && !server_time_value.is<JsonObjectConst>()) {
        error = "server_time provisioning section must be an object";
        return false;
    }
    if (has_server_time) {
        JsonObjectConst server_time = server_time_value.as<JsonObjectConst>();
        JsonVariantConst timestamp = server_time["timestamp"];
        if (timestamp.isUnbound() || !timestamp.is<int64_t>()) {
            error = "server timestamp must be an integer";
            return false;
        }
        output.server_timestamp_ms = timestamp.as<int64_t>();
        JsonVariantConst timezone_offset = server_time["timezone_offset"];
        if (!timezone_offset.isUnbound()) {
            if (!timezone_offset.is<int32_t>()) {
                error = "server timezone offset must be an integer";
                return false;
            }
            output.timezone_offset_minutes = timezone_offset.as<int32_t>();
        }
        if (output.timezone_offset_minutes < -24 * 60 ||
            output.timezone_offset_minutes > 24 * 60) {
            error = "server timezone offset is outside the supported range";
            return false;
        }
        if (output.server_timestamp_ms <= 0) {
            error = "server timestamp must be positive";
            return false;
        }
        output.has_server_time = true;
    }

    if (!output.websocket.present && !output.mqtt.present && !output.activation.present &&
        !output.firmware.present) {
        error = "provisioning response contains no supported configuration";
        return false;
    }
    error.clear();
    return true;
}

bool Provisioning::compareVersions(const std::string& left, const std::string& right,
                                   int& comparison) {
    std::vector<uint32_t> left_numbers;
    std::vector<uint32_t> right_numbers;
    if (!parseVersion(left, left_numbers) || !parseVersion(right, right_numbers)) {
        comparison = 0;
        return false;
    }
    const size_t count = std::max(left_numbers.size(), right_numbers.size());
    for (size_t index = 0; index < count; ++index) {
        const uint32_t left_value = index < left_numbers.size() ? left_numbers[index] : 0;
        const uint32_t right_value = index < right_numbers.size() ? right_numbers[index] : 0;
        if (left_value < right_value) {
            comparison = -1;
            return true;
        }
        if (left_value > right_value) {
            comparison = 1;
            return true;
        }
    }
    comparison = 0;
    return true;
}

bool ArduinoProvisioningClient::fetch(NetworkClient& network,
                                      const ProvisioningRequest& request,
                                      ProvisioningResult& output, std::string& error,
                                      size_t max_json_bytes) {
    if (!validHttpUrl(request.url) ||
        !validHeaderValue(request.device_id, 128) ||
        !validHeaderValue(request.client_id, 128) ||
        !validHeaderValue(request.user_agent, 256, false) ||
        !validHeaderValue(request.language, 64, false) ||
        request.body_json.size() > max_json_bytes ||
        request.activation_version < 1 || request.activation_version > 2 ||
        request.connect_timeout_ms == 0 || request.connect_timeout_ms > 600000 ||
        request.read_timeout_ms == 0 ||
        request.read_timeout_ms > std::numeric_limits<uint16_t>::max() ||
        max_json_bytes < 256) {
        error = "invalid provisioning request";
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(static_cast<int32_t>(request.connect_timeout_ms));
    http.setTimeout(static_cast<uint16_t>(
        std::min<uint32_t>(request.read_timeout_ms, std::numeric_limits<uint16_t>::max())));
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(network, String(request.url.c_str()))) {
        error = "failed to initialize HTTP provisioning request";
        return false;
    }
    http.addHeader("Activation-Version",
                   String(static_cast<unsigned int>(request.activation_version)));
    http.addHeader("Device-Id", String(request.device_id.c_str()));
    http.addHeader("Client-Id", String(request.client_id.c_str()));
    http.addHeader("User-Agent", String(request.user_agent.c_str()));
    http.addHeader("Accept-Language", String(request.language.c_str()));
    http.addHeader("Content-Type", "application/json");

    const int status = request.body_json.empty()
                           ? http.GET()
                           : http.POST(reinterpret_cast<uint8_t*>(
                                           const_cast<char*>(request.body_json.data())),
                                       request.body_json.size());
    if (status != HTTP_CODE_OK) {
        error = "provisioning HTTP status " + std::to_string(status);
        http.end();
        return false;
    }
    const int content_length = http.getSize();
    if (content_length > 0 && static_cast<size_t>(content_length) > max_json_bytes) {
        error = "provisioning HTTP response exceeds the configured limit";
        http.end();
        return false;
    }

    BoundedStringStream response(max_json_bytes);
    const int written = http.writeToStream(&response);
    http.end();
    if (written < 0 || response.overflowed()) {
        error = response.overflowed() ? "provisioning HTTP response exceeded its size limit"
                                      : "failed to read provisioning HTTP response";
        return false;
    }
    return Provisioning::parse(response.value(), output, error, max_json_bytes);
}

const char* ArduinoOfficialService::rootCACertificate() {
    return kOfficialRootCa;
}

bool ArduinoOfficialService::configure(ClientConfig& config,
                                       ProvisioningResult& provisioning,
                                       std::string& error,
                                       const OfficialServiceOptions& options) {
    error.clear();
    provisioning = {};
    if (WiFi.status() != WL_CONNECTED) {
        error = "WiFi must be connected before official provisioning";
        return false;
    }
    if (options.provisioning_url == nullptr || options.provisioning_url[0] == '\0' ||
        !validHttpUrl(options.provisioning_url) ||
        options.board_type == nullptr || options.board_type[0] == '\0' ||
        options.board_name == nullptr || options.board_name[0] == '\0' ||
        options.language == nullptr || options.language[0] == '\0' ||
        options.request_timeout_ms == 0 ||
        options.request_timeout_ms > std::numeric_limits<uint16_t>::max()) {
        error = "official service options are invalid";
        return false;
    }
    if (!synchronizeClock(options, error)) {
        return false;
    }

    const std::string device_id = Esp32Identity::deviceId();
    const std::string client_id = Esp32Identity::persistentClientId();
    if (device_id.empty() || client_id.empty()) {
        error = "failed to obtain the ESP32 device identity";
        return false;
    }

    NetworkClientSecure tls;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    tls.setCACertBundle(x509CrtBundleStart,
                        static_cast<size_t>(x509CrtBundleEnd - x509CrtBundleStart));
#else
    tls.setCACert(kOfficialRootCa);
#endif

    ProvisioningRequest request;
    request.url = options.provisioning_url;
    request.device_id = device_id;
    request.client_id = client_id;
    request.user_agent = std::string(options.board_type) + "/" +
                         Esp32Identity::firmwareVersion();
    request.language = options.language;
    request.body_json = buildOfficialSystemInfo(device_id, client_id, options);
    request.activation_version = 1;
    request.connect_timeout_ms = options.request_timeout_ms;
    request.read_timeout_ms = options.request_timeout_ms;
    if (!ArduinoProvisioningClient::fetch(tls, request, provisioning, error)) {
        return false;
    }
    if (!provisioning.websocket.present) {
        error = "official provisioning response has no WebSocket configuration";
        return false;
    }

    if (provisioning.has_server_time && provisioning.server_timestamp_ms > 0) {
        timeval value{};
        value.tv_sec = static_cast<time_t>(provisioning.server_timestamp_ms / 1000);
        value.tv_usec = static_cast<suseconds_t>(
            (provisioning.server_timestamp_ms % 1000) * 1000);
        settimeofday(&value, nullptr);
    }

    config.websocket_url = provisioning.websocket.url;
    config.authorization = provisioning.websocket.token;
    config.device_id = device_id;
    config.client_id = client_id;
    config.protocol_version = provisioning.websocket.version;
    return true;
}

}  // namespace xiaozhi
