#include "Client.h"

#include <ArduinoJson.h>

#include <cstring>
#include <utility>
#include <vector>

namespace xiaozhi {
namespace {

std::string jsonString(JsonVariantConst value) {
    std::string output;
    serializeJson(value, output);
    return output;
}

bool getString(JsonObjectConst object, const char* key, std::string& output) {
    JsonVariantConst value = object[key];
    if (!value.is<const char*>()) {
        return false;
    }
    const JsonString string = value.as<JsonString>();
    output.assign(string.c_str(), string.size());
    return true;
}

}  // namespace

Client::Client(Transport& transport, Clock* clock)
    : transport_(transport), clock_(clock == nullptr ? static_cast<Clock&>(default_clock_) : *clock) {
    state_machine_.addListener(
        [this](State old_state, State new_state) { handleStateChange(old_state, new_state); });
}

Client::~Client() {
    end();
    transport_.setCallbacks({});
}

bool Client::begin(const ClientConfig& config, Callbacks callbacks) {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0) {
        return false;
    }
    if (begun_) {
        reportError(ErrorCode::InvalidState, "client is already initialized");
        return false;
    }

    std::string error;
    if (!Protocol::validateConfig(config, error)) {
        callbacks_ = std::move(callbacks);
        reportError(ErrorCode::InvalidConfiguration, error);
        return false;
    }

    config_ = config;
    callbacks_ = std::move(callbacks);
    // Reserve room for the outer {session_id,type,payload} envelope and escaping.
    mcp_server_.setMaxResponseBytes(config_.max_json_bytes - 384);
    installTransportCallbacks();
    clearSession();
    end_requested_ = false;
    begin_in_progress_ = true;
    begun_ = true;

    if (audio_port_ != nullptr) {
        audio_port_started_ = audio_port_->begin(
            config_.input_audio,
            [this](const uint8_t* opus, size_t size, uint32_t timestamp) {
                return sendAudio(opus, size, timestamp);
            });
        if (!audio_port_started_) {
            begun_ = false;
            begin_in_progress_ = false;
            transport_.setCallbacks({});
            reportError(ErrorCode::InvalidConfiguration, "attached audio port failed to start");
            return false;
        }
    }

    if (state_machine_.state() == State::Unknown) {
        if (!state_machine_.transitionTo(State::Starting) ||
            !state_machine_.transitionTo(State::Activating) ||
            !state_machine_.transitionTo(State::Idle)) {
            begun_ = false;
            begin_in_progress_ = false;
            if (audio_port_started_ && audio_port_ != nullptr) {
                audio_port_->end();
                audio_port_started_ = false;
            }
            transport_.setCallbacks({});
            reportError(ErrorCode::InvalidState, "failed to enter the initial idle state");
            return false;
        }
    } else if (state_machine_.state() != State::Idle) {
        begun_ = false;
        begin_in_progress_ = false;
        if (audio_port_started_ && audio_port_ != nullptr) {
            audio_port_->end();
            audio_port_started_ = false;
        }
        transport_.setCallbacks({});
        reportError(ErrorCode::InvalidState, "client cannot begin from its current state");
        return false;
    }
    begin_in_progress_ = false;
    if (end_requested_) {
        return false;
    }
    return true;
}

void Client::end() {
    if (dispatch_depth_ != 0 || user_callback_depth_ != 0 || begin_in_progress_) {
        end_requested_ = true;
        return;
    }
    if (!begun_) {
        // A deferred end can originate from an error callback before begin() has
        // committed the client. Never let that cancellation poison the next begin.
        end_requested_ = false;
        return;
    }
    if (ending_) {
        return;
    }
    ending_ = true;
    setCaptureEnabled(false);
    if (audio_port_started_ && audio_port_ != nullptr) {
        audio_port_->setCaptureEnabled(false);
        audio_port_->end();
        audio_port_started_ = false;
    }
    closing_ = true;
    if (transport_.connected()) {
        transport_.close();
    }
    closing_ = false;
    clearSession();
    const State current = state_machine_.state();
    if (current == State::Connecting || current == State::Listening ||
        current == State::Speaking) {
        state_machine_.transitionTo(State::Idle);
    }
    begun_ = false;
    end_requested_ = false;
    ending_ = false;
}

void Client::loop() {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0 || !begun_ || end_requested_) {
        return;
    }
    transport_.loop();
    if (end_requested_) {
        return;
    }
    if (audio_port_started_ && audio_port_ != nullptr) {
        audio_port_->loop();
    }
    if (end_requested_) {
        return;
    }

    if (pending_listening_start_ && state_machine_.state() == State::Listening &&
        (!audio_port_started_ || audio_port_ == nullptr || audio_port_->playbackIdle())) {
        pending_listening_start_ = false;
        if (!enterListeningOrClose()) {
            return;
        }
    }

    const uint64_t now = clock_.nowMs();
    if (awaiting_hello_ && now - handshake_started_ms_ >= config_.handshake_timeout_ms) {
        awaiting_hello_ = false;
        reportError(ErrorCode::HandshakeTimeout, "timed out waiting for the server hello");
        if (!end_requested_) {
            closeSession();
        }
        return;
    }
    if (session_ready_ && last_incoming_ms_ != 0 &&
        now - last_incoming_ms_ > config_.channel_timeout_ms) {
        reportError(ErrorCode::ChannelTimeout, "audio channel timed out");
        if (!end_requested_) {
            closeSession();
        }
    }
}

bool Client::startListening(ListeningMode mode) {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0) {
        return false;
    }
    if (end_requested_) {
        return false;
    }
    if (!begun_ || begin_in_progress_ || ending_) {
        reportError(ErrorCode::InvalidState, "client is not initialized");
        return false;
    }

    listening_mode_ = mode;
    const State current = state_machine_.state();
    if (current == State::Speaking) {
        if (!abortSpeaking(AbortReason::None)) {
            return false;
        }
        if (!state_machine_.transitionTo(State::Listening)) {
            reportError(ErrorCode::InvalidState, "cannot transition from speaking to listening");
            return false;
        }
        if (end_requested_ || !begun_ || state_machine_.state() != State::Listening) {
            return false;
        }
        return enterListeningOrClose();
    }
    if (current == State::Listening) {
        return enterListeningOrClose();
    }
    if (current != State::Idle) {
        reportError(ErrorCode::InvalidState,
                    std::string("cannot start listening from ") + xiaozhi::stateName(current));
        return false;
    }

    if (session_ready_ && transport_.connected()) {
        if (!state_machine_.transitionTo(State::Listening)) {
            reportError(ErrorCode::InvalidState, "cannot enter listening state");
            return false;
        }
        if (end_requested_ || !begun_ || state_machine_.state() != State::Listening) {
            return false;
        }
        return enterListeningOrClose();
    }

    if (!state_machine_.transitionTo(State::Connecting)) {
        reportError(ErrorCode::InvalidState, "cannot enter connecting state");
        return false;
    }
    if (end_requested_ || !begun_ || state_machine_.state() != State::Connecting) {
        return false;
    }
    return connectSession();
}

bool Client::stopListening() {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0) {
        return false;
    }
    if (end_requested_) {
        return false;
    }
    if (!begun_ || ending_ || state_machine_.state() != State::Listening) {
        reportError(ErrorCode::InvalidState, "stopListening requires the listening state");
        return false;
    }
    std::string message;
    if (!Protocol::makeStopListening(session_id_, message) || !sendText(message)) {
        return false;
    }
    const bool transitioned = state_machine_.transitionTo(State::Idle);
    return transitioned && !end_requested_;
}

bool Client::toggleChat() {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0) {
        return false;
    }
    if (end_requested_) {
        return false;
    }
    switch (state_machine_.state()) {
        case State::Idle:
            return startListening(ListeningMode::AutoStop);
        case State::Speaking:
            return abortSpeaking(AbortReason::None);
        case State::Listening:
            return closeSession();
        default:
            reportError(ErrorCode::InvalidState, "toggleChat is unavailable in the current state");
            return false;
    }
}

bool Client::abortSpeaking(AbortReason reason) {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0) {
        return false;
    }
    if (end_requested_) {
        return false;
    }
    if (!begun_ || ending_ || !session_ready_ ||
        state_machine_.state() != State::Speaking) {
        reportError(ErrorCode::InvalidState, "abortSpeaking requires an active speaking session");
        return false;
    }
    std::string message;
    return Protocol::makeAbort(session_id_, reason, message) && sendText(message) &&
           !end_requested_;
}

bool Client::wakeWordDetected(const std::string& wake_word) {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0) {
        return false;
    }
    if (end_requested_) {
        return false;
    }
    if (!begun_ || ending_ || wake_word.empty() || wake_word.size() > 256) {
        reportError(ErrorCode::InvalidConfiguration, "wake word must contain 1..256 characters");
        return false;
    }

    const State current = state_machine_.state();
    pending_wake_word_ = wake_word;
    if (current == State::Idle) {
        return startListening(ListeningMode::AutoStop);
    }
    if (current == State::Speaking) {
        if (!abortSpeaking(AbortReason::WakeWordDetected) ||
            !state_machine_.transitionTo(State::Listening)) {
            pending_wake_word_.clear();
            return false;
        }
        return enterListeningOrClose();
    }
    if (current == State::Listening) {
        std::string abort;
        if (!Protocol::makeAbort(session_id_, AbortReason::WakeWordDetected, abort) ||
            !sendText(abort)) {
            pending_wake_word_.clear();
            return false;
        }
        return enterListeningOrClose();
    }

    pending_wake_word_.clear();
    reportError(ErrorCode::InvalidState, "wake word cannot be handled in the current state");
    return false;
}

bool Client::closeSession() {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0) {
        return false;
    }
    if (!begun_ || ending_ || end_requested_) {
        return false;
    }
    closing_ = true;
    if (transport_.connected()) {
        transport_.close();
    }
    closing_ = false;
    if (end_requested_) {
        return false;
    }
    clearSession();
    const State current = state_machine_.state();
    if (current == State::Connecting || current == State::Listening ||
        current == State::Speaking) {
        const bool transitioned = state_machine_.transitionTo(State::Idle);
        return transitioned && !end_requested_;
    }
    return true;
}

bool Client::sendAudio(const uint8_t* opus, size_t size, uint32_t timestamp) {
    DispatchScope dispatch(*this);
    // Public control APIs are intentionally rejected from every user callback. In
    // particular, reporting this rejection through on_error would let an on_error
    // handler recursively call sendAudio until the stack is exhausted.
    if (user_callback_depth_ != 0) {
        return false;
    }
    const State current = state_machine_.state();
    const bool allowed = current == State::Listening ||
                         (current == State::Speaking &&
                          listening_mode_ == ListeningMode::Realtime);
    if (end_requested_) {
        return false;
    }
    if (!begun_ || ending_ || !session_ready_ ||
        !transport_.connected() || !capture_enabled_ || !allowed) {
        reportError(ErrorCode::InvalidState,
                    "audio can only be sent while listening or in realtime speaking mode");
        return false;
    }

    std::vector<uint8_t> frame;
    std::string error;
    if (!Protocol::encodeAudio(config_.protocol_version, opus, size, timestamp,
                               config_.max_audio_payload_bytes, frame, error)) {
        reportError(ErrorCode::InvalidAudioFrame, error);
        return false;
    }
    if (!transport_.sendBinary(frame.data(), frame.size())) {
        if (!end_requested_) {
            reportError(ErrorCode::TransportSendFailed, "failed to send an audio frame");
        }
        return false;
    }
    return !end_requested_;
}

bool Client::sendMcp(const std::string& json_rpc_payload) {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0) {
        return false;
    }
    if (end_requested_) {
        return false;
    }
    if (!config_.enable_mcp) {
        reportError(ErrorCode::InvalidState, "MCP is disabled in ClientConfig");
        return false;
    }
    if (!begun_ || ending_ || !session_ready_) {
        reportError(ErrorCode::InvalidState, "MCP requires an active session");
        return false;
    }
    std::string message;
    std::string error;
    if (!Protocol::makeMcp(session_id_, json_rpc_payload, message, error)) {
        reportError(ErrorCode::McpError, error);
        return false;
    }
    return sendText(message);
}

bool Client::attachAudioPort(EncodedAudioPort* audio_port) {
    DispatchScope dispatch(*this);
    if (user_callback_depth_ != 0) {
        return false;
    }
    if (end_requested_) {
        return false;
    }
    if (begun_) {
        reportError(ErrorCode::InvalidState, "attachAudioPort must be called before begin");
        return false;
    }
    audio_port_ = audio_port;
    return true;
}

void Client::installTransportCallbacks() {
    TransportCallbacks callbacks;
    callbacks.on_open = [this]() { onTransportOpen(); };
    callbacks.on_text =
        [this](const uint8_t* data, size_t size) { onTransportText(data, size); };
    callbacks.on_binary =
        [this](const uint8_t* data, size_t size) { onTransportBinary(data, size); };
    callbacks.on_close = [this]() { onTransportClose(); };
    callbacks.on_error =
        [this](const std::string& message) { onTransportError(message); };
    transport_.setCallbacks(std::move(callbacks));
}

void Client::onTransportOpen() {
    DispatchScope dispatch(*this);
    if (!begun_ || end_requested_ || state_machine_.state() != State::Connecting) {
        return;
    }
    std::string hello;
    std::string error;
    if (!Protocol::makeHello(config_, hello, error) || !sendText(hello)) {
        if (end_requested_) {
            return;
        }
        if (!error.empty()) {
            reportError(ErrorCode::InvalidConfiguration, error);
        }
        if (!end_requested_) {
            closeSession();
        }
        return;
    }
    awaiting_hello_ = true;
    handshake_started_ms_ = clock_.nowMs();
    last_incoming_ms_ = handshake_started_ms_;
}

void Client::onTransportText(const uint8_t* data, size_t size) {
    DispatchScope dispatch(*this);
    if (!begun_ || end_requested_) {
        return;
    }
    if (data == nullptr || size == 0) {
        reportError(ErrorCode::InvalidMessage, "received an empty text message");
        return;
    }
    if (size > config_.max_json_bytes) {
        reportError(ErrorCode::JsonTooLarge, "incoming JSON exceeds the configured limit");
        return;
    }

    JsonDocument document;
    const DeserializationError json_error = deserializeJson(document, data, size);
    if (json_error || !document.is<JsonObject>()) {
        reportError(ErrorCode::InvalidJson,
                    json_error ? std::string("invalid incoming JSON: ") + json_error.c_str()
                               : "incoming JSON must be an object");
        return;
    }
    JsonObjectConst root = document.as<JsonObjectConst>();
    std::string type;
    if (!getString(root, "type", type) || type.size() > 64) {
        reportError(ErrorCode::InvalidMessage, "incoming JSON is missing a string type");
        return;
    }
    if (type == "hello") {
        if (!awaiting_hello_ || state_machine_.state() != State::Connecting) {
            reportError(ErrorCode::InvalidMessage, "received an unexpected server hello");
            return;
        }
        ServerHello hello;
        std::string error;
        if (!Protocol::parseServerHello(data, size, hello, error)) {
            reportError(ErrorCode::InvalidMessage, error);
            closeSession();
            return;
        }
        awaiting_hello_ = false;
        session_ready_ = true;
        session_id_ = std::move(hello.session_id);
        server_audio_format_ = hello.audio;
        last_incoming_ms_ = clock_.nowMs();
        if (!state_machine_.transitionTo(State::Listening)) {
            if (!end_requested_) {
                closeSession();
            }
            return;
        }
        if (end_requested_) {
            return;
        }
        if (!enterListening() && !end_requested_) {
            closeSession();
        }
        return;
    }

    if (!session_ready_) {
        reportError(ErrorCode::InvalidMessage, "received a session message before server hello");
        return;
    }

    JsonVariantConst incoming_session = root["session_id"];
    if (!incoming_session.isNull()) {
        if (!incoming_session.is<const char*>()) {
            reportError(ErrorCode::InvalidMessage, "session_id must be a string");
            return;
        }
        const JsonString json_id = incoming_session.as<JsonString>();
        const std::string incoming_id(json_id.c_str(), json_id.size());
        if (incoming_id.size() > 128) {
            reportError(ErrorCode::InvalidMessage, "session_id exceeds 128 characters");
            return;
        }
        if (!session_id_.empty() && incoming_id != session_id_) {
            reportError(ErrorCode::InvalidMessage, "message belongs to a different session");
            return;
        }
        if (session_id_.empty() && incoming_id.size() <= 128) {
            session_id_ = incoming_id;
        }
    }
    last_incoming_ms_ = clock_.nowMs();

    if (type == "tts") {
        std::string tts_state;
        if (!getString(root, "state", tts_state)) {
            reportError(ErrorCode::InvalidMessage, "TTS message is missing state");
            return;
        }
        if (tts_state == "start") {
            const State current = state_machine_.state();
            if (current == State::Listening || current == State::Idle) {
                state_machine_.transitionTo(State::Speaking);
            }
        } else if (tts_state == "stop") {
            if (state_machine_.state() == State::Speaking) {
                if (listening_mode_ == ListeningMode::ManualStop) {
                    state_machine_.transitionTo(State::Idle);
                } else if (state_machine_.transitionTo(State::Listening)) {
                    if (end_requested_) {
                        return;
                    }
                    if (audio_port_started_ && audio_port_ != nullptr &&
                        !audio_port_->playbackIdle()) {
                        pending_listening_start_ = true;
                    } else {
                        enterListeningOrClose();
                    }
                }
            }
        } else if (tts_state == "sentence_start") {
            Event event;
            event.type = EventType::TtsSentence;
            getString(root, "text", event.text);
            event.json.assign(reinterpret_cast<const char*>(data), size);
            emitEvent(std::move(event));
        }
    } else if (type == "stt") {
        Event event;
        event.type = EventType::Stt;
        getString(root, "text", event.text);
        event.json.assign(reinterpret_cast<const char*>(data), size);
        emitEvent(std::move(event));
    } else if (type == "llm") {
        Event event;
        event.type = EventType::Emotion;
        getString(root, "emotion", event.emotion);
        event.json.assign(reinterpret_cast<const char*>(data), size);
        emitEvent(std::move(event));
    } else if (type == "alert") {
        Event event;
        event.type = EventType::Alert;
        if (!getString(root, "status", event.status) ||
            !getString(root, "message", event.text) ||
            !getString(root, "emotion", event.emotion)) {
            reportError(ErrorCode::InvalidMessage,
                        "alert requires string status, message, and emotion fields");
            return;
        }
        event.json.assign(reinterpret_cast<const char*>(data), size);
        emitEvent(std::move(event));
    } else if (type == "mcp") {
        if (!config_.enable_mcp) {
            reportError(ErrorCode::InvalidMessage, "received MCP while MCP is disabled");
            return;
        }
        JsonVariantConst payload = root["payload"];
        if (!payload.is<JsonObjectConst>()) {
            reportError(ErrorCode::InvalidMessage, "MCP envelope is missing an object payload");
            return;
        }
        const std::string request = jsonString(payload);
        std::string response;
        std::string error;
        beginUserCallback();
        const bool handled = mcp_server_.handle(request, response, error);
        endUserCallback();
        if (end_requested_) {
            return;
        }
        if (!handled) {
            reportError(ErrorCode::McpError, error);
            return;
        }
        if (!begun_ || end_requested_ || !session_ready_) {
            return;
        }
        if (!response.empty()) {
            sendMcp(response);
        }
    } else if (type == "system") {
        std::string command;
        if (getString(root, "command", command) && command == "reboot") {
            Event event;
            event.type = EventType::RebootRequested;
            event.json.assign(reinterpret_cast<const char*>(data), size);
            emitEvent(std::move(event));
        }
    } else if (type == "custom") {
        Event event;
        event.type = EventType::Custom;
        event.json = root["payload"].isNull() ? std::string() : jsonString(root["payload"]);
        emitEvent(std::move(event));
    } else if (type == "goodbye") {
        std::string goodbye_session;
        if (!getString(root, "session_id", goodbye_session) || goodbye_session == session_id_) {
            closeSession();
        }
    } else {
        Event event;
        event.type = EventType::UnknownMessage;
        event.json.assign(reinterpret_cast<const char*>(data), size);
        emitEvent(std::move(event));
    }
}

void Client::onTransportBinary(const uint8_t* data, size_t size) {
    DispatchScope dispatch(*this);
    if (!begun_ || end_requested_ || !session_ready_) {
        return;
    }
    AudioFrame frame;
    std::string error;
    if (!Protocol::decodeAudio(config_.protocol_version, data, size, server_audio_format_,
                               config_.max_audio_payload_bytes, frame, error)) {
        reportError(ErrorCode::InvalidAudioFrame, error);
        return;
    }
    last_incoming_ms_ = clock_.nowMs();
    if (!config_.deliver_audio_outside_speaking && state_machine_.state() != State::Speaking) {
        return;
    }
    if (callbacks_.on_audio) {
        beginUserCallback();
        callbacks_.on_audio(frame);
        endUserCallback();
        if (end_requested_ || !begun_ || !session_ready_) {
            return;
        }
    }
    if (audio_port_started_ && audio_port_ != nullptr) {
        audio_port_->play(frame);
    }
}

void Client::onTransportClose() {
    DispatchScope dispatch(*this);
    if (!begun_ || end_requested_) {
        return;
    }
    const bool expected = closing_;
    clearSession();
    const State current = state_machine_.state();
    if (current == State::Connecting || current == State::Listening ||
        current == State::Speaking) {
        state_machine_.transitionTo(State::Idle);
    }
    if (!expected && !end_requested_) {
        reportError(ErrorCode::TransportDisconnected, "WebSocket connection closed");
    }
}

void Client::onTransportError(const std::string& message) {
    DispatchScope dispatch(*this);
    if (begun_ && !end_requested_) {
        reportError(ErrorCode::TransportDisconnected, message);
        if (end_requested_) {
            return;
        }
        const State current = state_machine_.state();
        if (current == State::Connecting || current == State::Listening ||
            current == State::Speaking || session_ready_) {
            closeSession();
        }
    }
}

bool Client::connectSession() {
    if (end_requested_) {
        return false;
    }
    std::string pending_wake_word = std::move(pending_wake_word_);
    clearSession();
    pending_wake_word_ = std::move(pending_wake_word);
    TransportRequest request;
    request.url = config_.websocket_url;
    if (!config_.authorization.empty()) {
        std::string authorization = config_.authorization;
        if (authorization.find(' ') == std::string::npos) {
            authorization = "Bearer " + authorization;
        }
        request.headers.emplace_back("Authorization", std::move(authorization));
    }
    request.headers.emplace_back("Protocol-Version",
                                 std::to_string(config_.protocol_version));
    request.headers.emplace_back("Device-Id", config_.device_id);
    request.headers.emplace_back("Client-Id", config_.client_id);
    if (!config_.user_agent.empty()) {
        request.headers.emplace_back("User-Agent", config_.user_agent);
    }

    handshake_started_ms_ = clock_.nowMs();
    if (!transport_.connect(request)) {
        if (end_requested_) {
            return false;
        }
        if (state_machine_.state() == State::Connecting) {
            state_machine_.transitionTo(State::Idle);
        }
        if (end_requested_) {
            return false;
        }
        reportError(ErrorCode::TransportConnectFailed, "WebSocket connect request failed");
        return false;
    }
    return !end_requested_;
}

bool Client::enterListening() {
    if (end_requested_) {
        return false;
    }
    if (!session_ready_ || state_machine_.state() != State::Listening) {
        reportError(ErrorCode::InvalidState, "session is not ready for listening");
        return false;
    }

    if (!pending_wake_word_.empty()) {
        std::string wake_message;
        const std::string wake_word = std::move(pending_wake_word_);
        pending_wake_word_.clear();
        if (!Protocol::makeWakeWordDetected(session_id_, wake_word, wake_message) ||
            !sendText(wake_message)) {
            return false;
        }
        if (end_requested_) {
            return false;
        }
    }

    std::string message;
    if (!Protocol::makeStartListening(session_id_, listening_mode_, message) ||
        !sendText(message)) {
        return false;
    }
    setCaptureEnabled(true);
    return !end_requested_ && begun_ && session_ready_ &&
           state_machine_.state() == State::Listening;
}

bool Client::enterListeningOrClose() {
    if (enterListening()) {
        return true;
    }
    if (!end_requested_) {
        closeSession();
    }
    return false;
}

bool Client::sendText(const std::string& text) {
    if (end_requested_) {
        return false;
    }
    if (!transport_.connected() || text.empty() || text.size() > config_.max_json_bytes) {
        reportError(ErrorCode::TransportSendFailed,
                    "text message is empty, too large, or transport is disconnected");
        return false;
    }
    if (!transport_.sendText(reinterpret_cast<const uint8_t*>(text.data()), text.size())) {
        if (!end_requested_) {
            reportError(ErrorCode::TransportSendFailed, "failed to send a text message");
        }
        return false;
    }
    return !end_requested_;
}

void Client::clearSession() {
    awaiting_hello_ = false;
    session_ready_ = false;
    handshake_started_ms_ = 0;
    last_incoming_ms_ = 0;
    session_id_.clear();
    pending_wake_word_.clear();
    pending_listening_start_ = false;
    server_audio_format_ = AudioFormat{24000, 60, 1};
}

void Client::handleStateChange(State old_state, State new_state) {
    DispatchScope dispatch(*this);
    const bool may_keep_capture =
        new_state == State::Listening ||
        (new_state == State::Speaking && listening_mode_ == ListeningMode::Realtime);
    if (!may_keep_capture) {
        setCaptureEnabled(false);
    }
    if (new_state != State::Listening) {
        pending_listening_start_ = false;
    }
    if (callbacks_.on_state_changed) {
        beginUserCallback();
        callbacks_.on_state_changed(old_state, new_state);
        endUserCallback();
    }
}

void Client::setCaptureEnabled(bool enabled) {
    DispatchScope dispatch(*this);
    if (capture_enabled_ == enabled) {
        return;
    }
    capture_enabled_ = enabled;
    if (audio_port_started_ && audio_port_ != nullptr) {
        audio_port_->setCaptureEnabled(enabled);
    }
    if (callbacks_.on_capture) {
        beginUserCallback();
        callbacks_.on_capture(enabled, config_.input_audio);
        endUserCallback();
    }
}

void Client::emitEvent(Event event) {
    DispatchScope dispatch(*this);
    if (callbacks_.on_event) {
        beginUserCallback();
        callbacks_.on_event(event);
        endUserCallback();
    }
}

void Client::reportError(ErrorCode code, const std::string& message) {
    DispatchScope dispatch(*this);
    if (reporting_error_) {
        return;
    }
    if (callbacks_.on_error) {
        reporting_error_ = true;
        beginUserCallback();
        callbacks_.on_error(code, message);
        endUserCallback();
        reporting_error_ = false;
    }
}

void Client::beginDispatch() {
    ++dispatch_depth_;
}

void Client::endDispatch() {
    if (dispatch_depth_ != 0) {
        --dispatch_depth_;
    }
    finishDeferredEnd();
}

void Client::beginUserCallback() {
    ++user_callback_depth_;
}

void Client::endUserCallback() {
    if (user_callback_depth_ != 0) {
        --user_callback_depth_;
    }
    finishDeferredEnd();
}

void Client::finishDeferredEnd() {
    if (dispatch_depth_ == 0 && user_callback_depth_ == 0 && end_requested_ &&
        !begin_in_progress_ && !ending_) {
        end_requested_ = false;
        end();
    }
}

}  // namespace xiaozhi
