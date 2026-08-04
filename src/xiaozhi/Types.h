#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Emotion.h"

namespace xiaozhi {

enum class State : uint8_t {
    Unknown,
    Starting,
    WifiConfiguring,
    Idle,
    Connecting,
    Listening,
    Speaking,
    Upgrading,
    Activating,
    AudioTesting,
    FatalError,
};

enum class ListeningMode : uint8_t {
    AutoStop,
    ManualStop,
    Realtime,
};

enum class AbortReason : uint8_t {
    None,
    WakeWordDetected,
};

enum class ErrorCode : uint8_t {
    None,
    InvalidConfiguration,
    InvalidState,
    TransportConnectFailed,
    TransportDisconnected,
    TransportSendFailed,
    HandshakeTimeout,
    ChannelTimeout,
    JsonTooLarge,
    InvalidJson,
    InvalidMessage,
    InvalidAudioFrame,
    McpError,
};

enum class EventType : uint8_t {
    Stt,
    TtsSentence,
    Emotion,
    Alert,
    Custom,
    RebootRequested,
    UnknownMessage,
};

struct AudioFormat {
    uint32_t sample_rate = 16000;
    uint16_t frame_duration_ms = 60;
    uint8_t channels = 1;
};

struct AudioFrame {
    AudioFormat format;
    uint32_t timestamp = 0;
    std::vector<uint8_t> opus;
};

struct Event {
    EventType type = EventType::UnknownMessage;
    std::string text;
    std::string status;
    // Raw protocol value retained for compatibility and custom server values.
    std::string emotion;
    std::string json;
    // Kept after the original fields so existing aggregate initialization remains valid.
    Emotion emotion_type = Emotion::Unknown;
};

struct ClientConfig {
    std::string websocket_url;
    std::string authorization;
    std::string device_id;
    std::string client_id;
    std::string user_agent = "xiaozhi-arduino/2.4.0";

    uint8_t protocol_version = 1;
    AudioFormat input_audio;
    uint32_t handshake_timeout_ms = 10000;
    uint32_t channel_timeout_ms = 120000;
    size_t max_json_bytes = 8192;
    size_t max_audio_payload_bytes = 4096;
    bool enable_mcp = true;
    // Server AEC uses protocol-v2 timestamps. A compatible audio port should
    // return the timestamp of a downlink frame after it reaches playback I/O.
    bool enable_server_aec = false;
    bool deliver_audio_outside_speaking = false;
};

struct Callbacks {
    std::function<void(State old_state, State new_state)> on_state_changed;
    std::function<void(const Event& event)> on_event;
    std::function<void(const AudioFrame& frame)> on_audio;
    std::function<void(bool enabled, const AudioFormat& format)> on_capture;
    std::function<void(ErrorCode code, const std::string& message)> on_error;
};

inline const char* stateName(State state) {
    switch (state) {
        case State::Unknown:
            return "unknown";
        case State::Starting:
            return "starting";
        case State::WifiConfiguring:
            return "wifi_configuring";
        case State::Idle:
            return "idle";
        case State::Connecting:
            return "connecting";
        case State::Listening:
            return "listening";
        case State::Speaking:
            return "speaking";
        case State::Upgrading:
            return "upgrading";
        case State::Activating:
            return "activating";
        case State::AudioTesting:
            return "audio_testing";
        case State::FatalError:
            return "fatal_error";
    }
    return "invalid_state";
}

inline const char* errorName(ErrorCode code) {
    switch (code) {
        case ErrorCode::None:
            return "none";
        case ErrorCode::InvalidConfiguration:
            return "invalid_configuration";
        case ErrorCode::InvalidState:
            return "invalid_state";
        case ErrorCode::TransportConnectFailed:
            return "transport_connect_failed";
        case ErrorCode::TransportDisconnected:
            return "transport_disconnected";
        case ErrorCode::TransportSendFailed:
            return "transport_send_failed";
        case ErrorCode::HandshakeTimeout:
            return "handshake_timeout";
        case ErrorCode::ChannelTimeout:
            return "channel_timeout";
        case ErrorCode::JsonTooLarge:
            return "json_too_large";
        case ErrorCode::InvalidJson:
            return "invalid_json";
        case ErrorCode::InvalidMessage:
            return "invalid_message";
        case ErrorCode::InvalidAudioFrame:
            return "invalid_audio_frame";
        case ErrorCode::McpError:
            return "mcp_error";
    }
    return "unknown_error";
}

}  // namespace xiaozhi
