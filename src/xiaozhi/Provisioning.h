#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Types.h"

class NetworkClient;

namespace xiaozhi {

constexpr char kOfficialProvisioningUrl[] = "https://api.tenclass.net/xiaozhi/ota/";

struct WebSocketProvision {
    bool present = false;
    std::string url;
    std::string token;
    uint8_t version = 1;
};

struct MqttProvision {
    bool present = false;
    std::string endpoint;
    std::string client_id;
    std::string username;
    std::string password;
    std::string publish_topic;
    uint32_t keepalive_seconds = 240;
};

struct ActivationProvision {
    bool present = false;
    std::string message;
    std::string code;
    std::string challenge;
    uint32_t timeout_ms = 30000;
};

struct FirmwareProvision {
    bool present = false;
    std::string version;
    std::string url;
    bool force = false;
};

struct ProvisioningResult {
    WebSocketProvision websocket;
    MqttProvision mqtt;
    ActivationProvision activation;
    FirmwareProvision firmware;
    bool has_server_time = false;
    int64_t server_timestamp_ms = 0;
    int32_t timezone_offset_minutes = 0;
};

class Provisioning {
public:
    static bool parse(const uint8_t* data, size_t size, ProvisioningResult& output,
                      std::string& error, size_t max_json_bytes = 16384);
    static bool parse(const std::string& json, ProvisioningResult& output,
                      std::string& error, size_t max_json_bytes = 16384) {
        return parse(reinterpret_cast<const uint8_t*>(json.data()), json.size(), output, error,
                     max_json_bytes);
    }

    // Returns -1 when left < right, 0 when equal, and 1 when left > right.
    static bool compareVersions(const std::string& left, const std::string& right,
                                int& comparison);
};

struct ProvisioningRequest {
    // The official Xiaozhi configuration service is the safe default. Callers
    // can still replace this URL when using a self-hosted service.
    std::string url = kOfficialProvisioningUrl;
    std::string device_id;
    std::string client_id;
    std::string user_agent = "xiaozhi-arduino/2.4.0";
    std::string language = "zh-CN";
    std::string body_json;
    uint8_t activation_version = 1;
    uint32_t connect_timeout_ms = 10000;
    uint32_t read_timeout_ms = 10000;
};

// HTTP is an adapter: the caller supplies a configured plain or TLS NetworkClient.
// For HTTPS, configure CA verification and setHandshakeTimeout(seconds) on
// NetworkClientSecure before calling fetch(). HTTP connect/read limits do not
// set the TLS handshake timeout. This API is synchronous; keep it off the
// Runtime/audio tasks. Read timeouts bound inactivity, not the entire response.
class ArduinoProvisioningClient {
public:
    static bool fetch(NetworkClient& network, const ProvisioningRequest& request,
                      ProvisioningResult& output, std::string& error,
                      size_t max_json_bytes = 16384);
};

struct OfficialServiceOptions {
    // Leave this unchanged to use Xiaozhi's official service. A self-hosted
    // compatible provisioning endpoint can be supplied explicitly.
    const char* provisioning_url = kOfficialProvisioningUrl;
    const char* board_type = "generic-esp32-arduino";
    const char* board_name = "Generic ESP32 Arduino";
    const char* language = "zh-CN";
    const char* primary_ntp_server = "ntp.aliyun.com";
    const char* secondary_ntp_server = "pool.ntp.org";
    uint32_t time_sync_timeout_ms = 15000;
    // Per TCP connect, TLS handshake (rounded up to seconds), and HTTP read
    // inactivity timeout; not a total wall-clock deadline including DNS/NTP.
    uint32_t request_timeout_ms = 15000;
};

// ESP32 Arduino convenience path for the official Xiaozhi service. It obtains
// the stable device identity, synchronizes time, securely provisions the
// WebSocket URL/token/version, and fills ClientConfig. Custom servers continue
// to override OfficialServiceOptions::provisioning_url or use
// ArduinoProvisioningClient::fetch() with a fully explicit request.
class ArduinoOfficialService {
public:
    static bool configure(ClientConfig& config, ProvisioningResult& provisioning,
                          std::string& error,
                          const OfficialServiceOptions& options = OfficialServiceOptions{});
    static const char* provisioningUrl() { return kOfficialProvisioningUrl; }
    static const char* rootCACertificate();
};

}  // namespace xiaozhi
