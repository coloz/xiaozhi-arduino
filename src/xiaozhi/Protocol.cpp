#include "Protocol.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace xiaozhi {
namespace {

constexpr size_t kProtocol2HeaderSize = 16;
constexpr size_t kProtocol3HeaderSize = 4;

uint16_t readBe16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) |
                                 static_cast<uint16_t>(data[1]));
}

uint32_t readBe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24U) |
           (static_cast<uint32_t>(data[1]) << 16U) |
           (static_cast<uint32_t>(data[2]) << 8U) | static_cast<uint32_t>(data[3]);
}

void writeBe16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8U);
    data[1] = static_cast<uint8_t>(value & 0xffU);
}

void writeBe32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value >> 24U);
    data[1] = static_cast<uint8_t>((value >> 16U) & 0xffU);
    data[2] = static_cast<uint8_t>((value >> 8U) & 0xffU);
    data[3] = static_cast<uint8_t>(value & 0xffU);
}

bool serializeDocument(JsonDocument& document, std::string& output) {
    output.clear();
    serializeJson(document, output);
    return !output.empty();
}

bool jsonStringEquals(JsonVariantConst value, const char* expected) {
    if (!value.is<const char*>()) {
        return false;
    }
    const JsonString string = value.as<JsonString>();
    const size_t expected_size = std::strlen(expected);
    return string.size() == expected_size &&
           std::memcmp(string.c_str(), expected, expected_size) == 0;
}

bool validSampleRate(uint32_t sample_rate) {
    return sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 ||
           sample_rate == 24000 || sample_rate == 48000;
}

bool validFrameDuration(uint16_t duration_ms) {
    return duration_ms == 10 || duration_ms == 20 || duration_ms == 40 || duration_ms == 60;
}

bool hasHeaderControlCharacter(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7fU;
    });
}

bool validWebSocketUrl(const std::string& url) {
    if (std::any_of(url.begin(), url.end(), [](unsigned char character) {
            return character <= 0x20U || character == 0x7fU;
        })) {
        return false;
    }
    const size_t scheme_size = url.rfind("ws://", 0) == 0 ? 5U : 6U;
    const size_t path = url.find('/', scheme_size);
    const std::string authority = url.substr(
        scheme_size, path == std::string::npos ? std::string::npos : path - scheme_size);
    if (authority.empty() || authority.find('@') != std::string::npos ||
        authority.find('[') != std::string::npos || authority.find(']') != std::string::npos ||
        authority.find('?') != std::string::npos || authority.find('#') != std::string::npos) {
        return false;
    }

    const size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        return authority.size() <= 253;
    }
    if (colon == 0 || colon > 253 || authority.find(':', colon + 1) != std::string::npos) {
        return false;  // ArduinoWebsockets 0.5.x URL parsing does not support IPv6 literals.
    }
    const std::string port_text = authority.substr(colon + 1);
    if (port_text.empty() || port_text.size() > 5 ||
        !std::all_of(port_text.begin(), port_text.end(),
                     [](unsigned char value) { return value >= '0' && value <= '9'; })) {
        return false;
    }
    uint32_t port = 0;
    for (char digit : port_text) {
        port = port * 10U + static_cast<uint32_t>(digit - '0');
    }
    return port >= 1 && port <= 65535;
}

}  // namespace

size_t Protocol::maximumAudioFrameBytes(uint8_t version,
                                        size_t maximum_payload_bytes) {
    if (version == 2) {
        return maximum_payload_bytes + kProtocol2HeaderSize;
    }
    if (version == 3) {
        return maximum_payload_bytes + kProtocol3HeaderSize;
    }
    return maximum_payload_bytes;
}

bool Protocol::validateConfig(const ClientConfig& config, std::string& error) {
    if (config.websocket_url.size() > 2048 || hasHeaderControlCharacter(config.websocket_url) ||
        (config.websocket_url.rfind("ws://", 0) != 0 &&
         config.websocket_url.rfind("wss://", 0) != 0) ||
        !validWebSocketUrl(config.websocket_url)) {
        error = "websocket_url is malformed, unsafe, or longer than 2048 bytes";
        return false;
    }
    if (config.device_id.empty() || config.device_id.size() > 128 ||
        hasHeaderControlCharacter(config.device_id)) {
        error = "device_id must contain 1..128 characters";
        return false;
    }
    if (config.client_id.empty() || config.client_id.size() > 128 ||
        hasHeaderControlCharacter(config.client_id)) {
        error = "client_id must contain 1..128 characters";
        return false;
    }
    if (config.authorization.size() > 2048 ||
        hasHeaderControlCharacter(config.authorization)) {
        error = "authorization is unsafe or longer than 2048 bytes";
        return false;
    }
    if (config.user_agent.size() > 256 || hasHeaderControlCharacter(config.user_agent)) {
        error = "user_agent is unsafe or longer than 256 bytes";
        return false;
    }
    if (config.protocol_version < 1 || config.protocol_version > 3) {
        error = "protocol_version must be 1, 2, or 3";
        return false;
    }
    if (config.enable_voice_barge_in && !config.enable_server_aec) {
        error = "voice barge-in requires server AEC";
        return false;
    }
    if (!validSampleRate(config.input_audio.sample_rate) ||
        !validFrameDuration(config.input_audio.frame_duration_ms) ||
        config.input_audio.channels != 1) {
        error = "input audio must be mono Opus at 8/12/16/24/48 kHz with a 10/20/40/60 ms frame";
        return false;
    }
    if (config.handshake_timeout_ms == 0 || config.channel_timeout_ms == 0) {
        error = "timeouts must be non-zero";
        return false;
    }
    if (config.max_json_bytes < 1024 || config.max_json_bytes > 16384 ||
        config.max_audio_payload_bytes == 0 || config.max_audio_payload_bytes > 8192) {
        error = "JSON limit must be 1024..16384 and audio payload limit 1..8192 bytes";
        return false;
    }
    if (config.include_raw_event_json &&
        (config.maximum_raw_event_json_bytes == 0 ||
         config.maximum_raw_event_json_bytes > config.max_json_bytes)) {
        error = "raw event JSON limit must fit within max_json_bytes";
        return false;
    }
    error.clear();
    return true;
}

bool Protocol::makeHello(const ClientConfig& config, std::string& output,
                         std::string& error) {
    if (!validateConfig(config, error)) {
        return false;
    }

    JsonDocument document;
    document["type"] = "hello";
    document["version"] = config.protocol_version;
    document["transport"] = "websocket";
    JsonObject features = document["features"].to<JsonObject>();
    features["mcp"] = config.enable_mcp;
    if (config.enable_server_aec) {
        features["aec"] = true;
    }
    JsonObject audio = document["audio_params"].to<JsonObject>();
    audio["format"] = "opus";
    audio["sample_rate"] = config.input_audio.sample_rate;
    audio["channels"] = config.input_audio.channels;
    audio["frame_duration"] = config.input_audio.frame_duration_ms;

    if (!serializeDocument(document, output)) {
        error = "failed to serialize hello message";
        return false;
    }
    error.clear();
    return true;
}

bool Protocol::parseServerHello(const uint8_t* data, size_t size, ServerHello& output,
                                std::string& error) {
    if (data == nullptr || size == 0) {
        error = "empty server hello";
        return false;
    }

    JsonDocument document;
    const DeserializationError json_error = deserializeJson(document, data, size);
    if (json_error) {
        error = std::string("invalid server hello JSON: ") + json_error.c_str();
        return false;
    }

    return parseServerHello(document.as<JsonObjectConst>(), output, error);
}

bool Protocol::parseServerHello(JsonObjectConst root, ServerHello& output,
                                std::string& error) {
    if (root.isNull() || !jsonStringEquals(root["type"], "hello")) {
        error = "server hello has an invalid type";
        return false;
    }
    if (!jsonStringEquals(root["transport"], "websocket")) {
        error = "server hello has an unsupported transport";
        return false;
    }
    const char* session_id = "";
    size_t session_length = 0;
    if (!root["session_id"].isNull()) {
        if (!root["session_id"].is<const char*>()) {
            error = "server hello session_id must be a string when present";
            return false;
        }
        const JsonString json_session_id = root["session_id"].as<JsonString>();
        session_id = json_session_id.c_str();
        session_length = json_session_id.size();
        if (session_length > 128 ||
            std::any_of(session_id, session_id + session_length, [](unsigned char character) {
                return character < 0x20U || character == 0x7fU;
            })) {
            error = "server hello session_id is unsafe or exceeds 128 characters";
            return false;
        }
    }

    AudioFormat audio{24000, 60, 1};
    JsonObjectConst audio_json = root["audio_params"].as<JsonObjectConst>();
    if (!audio_json.isNull()) {
        if (!audio_json["format"].isNull() &&
            !jsonStringEquals(audio_json["format"], "opus")) {
            error = "server requested a non-Opus audio format";
            return false;
        }
        if (!audio_json["sample_rate"].isNull() &&
            !audio_json["sample_rate"].is<uint32_t>()) {
            error = "server sample_rate must be an integer";
            return false;
        }
        if (audio_json["sample_rate"].is<uint32_t>()) {
            audio.sample_rate = audio_json["sample_rate"].as<uint32_t>();
        }
        if (!audio_json["frame_duration"].isNull() &&
            !audio_json["frame_duration"].is<uint16_t>()) {
            error = "server frame_duration must be an integer";
            return false;
        }
        if (audio_json["frame_duration"].is<uint16_t>()) {
            audio.frame_duration_ms = audio_json["frame_duration"].as<uint16_t>();
        }
        if (!audio_json["channels"].isNull() && !audio_json["channels"].is<uint8_t>()) {
            error = "server channels must be an integer";
            return false;
        }
        if (audio_json["channels"].is<uint8_t>()) {
            audio.channels = audio_json["channels"].as<uint8_t>();
        }
    }
    if (!validSampleRate(audio.sample_rate) || !validFrameDuration(audio.frame_duration_ms) ||
        audio.channels != 1) {
        error = "server hello contains unsupported audio parameters";
        return false;
    }

    output.session_id.assign(session_id, session_length);
    output.audio = audio;
    error.clear();
    return true;
}

bool Protocol::makeStartListening(const std::string& session_id, ListeningMode mode,
                                  std::string& output) {
    JsonDocument document;
    document["session_id"] = session_id;
    document["type"] = "listen";
    document["state"] = "start";
    switch (mode) {
        case ListeningMode::AutoStop:
            document["mode"] = "auto";
            break;
        case ListeningMode::ManualStop:
            document["mode"] = "manual";
            break;
        case ListeningMode::Realtime:
            document["mode"] = "realtime";
            break;
    }
    return serializeDocument(document, output);
}

bool Protocol::makeStopListening(const std::string& session_id, std::string& output) {
    JsonDocument document;
    document["session_id"] = session_id;
    document["type"] = "listen";
    document["state"] = "stop";
    return serializeDocument(document, output);
}

bool Protocol::makeWakeWordDetected(const std::string& session_id,
                                    const std::string& wake_word, std::string& output) {
    JsonDocument document;
    document["session_id"] = session_id;
    document["type"] = "listen";
    document["state"] = "detect";
    document["text"] = wake_word;
    return serializeDocument(document, output);
}

bool Protocol::makeAbort(const std::string& session_id, AbortReason reason,
                         std::string& output) {
    JsonDocument document;
    document["session_id"] = session_id;
    document["type"] = "abort";
    if (reason == AbortReason::WakeWordDetected) {
        document["reason"] = "wake_word_detected";
    }
    return serializeDocument(document, output);
}

bool Protocol::makeMcp(const std::string& session_id, const std::string& payload,
                       std::string& output, std::string& error) {
    JsonDocument payload_document;
    const DeserializationError json_error = deserializeJson(payload_document, payload);
    if (json_error || !payload_document.is<JsonObject>()) {
        error = "MCP payload must be a JSON object";
        return false;
    }

    JsonDocument document;
    document["session_id"] = session_id;
    document["type"] = "mcp";
    document["payload"].set(payload_document.as<JsonObjectConst>());
    if (!serializeDocument(document, output)) {
        error = "failed to serialize MCP envelope";
        return false;
    }
    error.clear();
    return true;
}

bool Protocol::encodeAudio(uint8_t version, const uint8_t* opus, size_t opus_size,
                           uint32_t timestamp, size_t max_payload,
                           std::vector<uint8_t>& output, std::string& error) {
    if ((opus == nullptr && opus_size != 0) || opus_size == 0 || opus_size > max_payload ||
        opus_size > std::numeric_limits<uint16_t>::max()) {
        error = "invalid Opus payload size";
        return false;
    }

    if (version == 1) {
        output.assign(opus, opus + opus_size);
    } else if (version == 2) {
        // Every byte is overwritten below. resize() reuses the Client-owned
        // capacity without first clearing the Opus payload on every frame.
        output.resize(kProtocol2HeaderSize + opus_size);
        writeBe16(output.data(), 2);
        writeBe16(output.data() + 2, 0);
        writeBe32(output.data() + 4, 0);
        writeBe32(output.data() + 8, timestamp);
        writeBe32(output.data() + 12, static_cast<uint32_t>(opus_size));
        std::memcpy(output.data() + kProtocol2HeaderSize, opus, opus_size);
    } else if (version == 3) {
        output.resize(kProtocol3HeaderSize + opus_size);
        output[0] = 0;
        output[1] = 0;
        writeBe16(output.data() + 2, static_cast<uint16_t>(opus_size));
        std::memcpy(output.data() + kProtocol3HeaderSize, opus, opus_size);
    } else {
        error = "unsupported binary protocol version";
        return false;
    }

    error.clear();
    return true;
}

bool Protocol::parseAudioView(uint8_t version, const uint8_t* data, size_t size,
                              const AudioFormat& format, size_t max_payload,
                              AudioFrameView& output, std::string& error) {
    if (data == nullptr || size == 0) {
        error = "empty audio frame";
        return false;
    }

    const uint8_t* payload = data;
    size_t payload_size = size;
    uint32_t timestamp = 0;

    if (version == 2) {
        if (size < kProtocol2HeaderSize) {
            error = "protocol v2 frame is shorter than its header";
            return false;
        }
        const uint16_t wire_version = readBe16(data);
        const uint16_t type = readBe16(data + 2);
        const uint32_t declared_size = readBe32(data + 12);
        if (wire_version != 2 || type != 0) {
            error = "protocol v2 frame has an invalid version or type";
            return false;
        }
        if (declared_size != size - kProtocol2HeaderSize) {
            error = "protocol v2 payload length mismatch";
            return false;
        }
        timestamp = readBe32(data + 8);
        payload = data + kProtocol2HeaderSize;
        payload_size = declared_size;
    } else if (version == 3) {
        if (size < kProtocol3HeaderSize) {
            error = "protocol v3 frame is shorter than its header";
            return false;
        }
        const uint16_t declared_size = readBe16(data + 2);
        if (data[0] != 0 || data[1] != 0) {
            error = "protocol v3 frame has an invalid type or reserved byte";
            return false;
        }
        if (declared_size != size - kProtocol3HeaderSize) {
            error = "protocol v3 payload length mismatch";
            return false;
        }
        payload = data + kProtocol3HeaderSize;
        payload_size = declared_size;
    } else if (version != 1) {
        error = "unsupported binary protocol version";
        return false;
    }

    if (payload_size == 0 || payload_size > max_payload) {
        error = "audio payload exceeds the configured limit";
        return false;
    }

    output.format = format;
    output.timestamp = timestamp;
    output.opus = payload;
    output.opus_size = payload_size;
    error.clear();
    return true;
}

bool Protocol::decodeAudio(uint8_t version, const uint8_t* data, size_t size,
                           const AudioFormat& format, size_t max_payload,
                           AudioFrame& output, std::string& error) {
    AudioFrameView view;
    if (!parseAudioView(version, data, size, format, max_payload, view, error)) {
        return false;
    }
    output.format = view.format;
    output.timestamp = view.timestamp;
    output.opus.assign(view.opus, view.opus + view.opus_size);
    return true;
}

}  // namespace xiaozhi
