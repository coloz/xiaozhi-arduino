#include "Client.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace xiaozhi {
namespace {

constexpr size_t kMaximumDeferredTextSends = 4;

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
    deferred_text_sends_.clear();
    deferred_text_sends_.reserve(kMaximumDeferredTextSends);
    deferred_text_bytes_ = 0;
    audio_send_buffer_.clear();
    if (config_.protocol_version != 1) {
        audio_send_buffer_.reserve(config_.max_audio_payload_bytes + 16);
    }
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
        audio_port_->cancelPlayback();
        audio_port_->end();
        audio_port_started_ = false;
    }
    // close() is also the Transport's quiescence barrier. Call it even after
    // connected() became false so a deferred terminal callback from the old
    // connection cannot arrive after a later reconnect.
    expected_close_ = true;
    transport_.close();
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
    // Give bounded uplink work the first timeslice. ArduinoWebsockets::poll()
    // drains every currently available message and can otherwise delay a ready
    // microphone packet during a downlink burst.
    if (audio_port_started_ && audio_port_ != nullptr) {
        audio_port_->loop();
    }
    if (end_requested_) {
        return;
    }
    // Let callbacks update Client state synchronously, but defer protocol sends
    // until loop() returns. The Transport never has to accept send() while its
    // own connect/loop/send stack is active.
    deferred_text_chain_count_ = 0;
    ++transport_call_depth_;
    transport_.loop();
    --transport_call_depth_;
    if (!drainDeferredTextSends()) {
        return;
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
        const std::string active_session = session_id_;
        if (!abortSpeaking(AbortReason::None)) {
            return false;
        }
        if (end_requested_ || !begun_ || !session_ready_ ||
            session_id_ != active_session) {
            return false;
        }
        const State state_after_abort = state_machine_.state();
        if (state_after_abort == State::Listening ||
            (state_after_abort == State::Speaking && !downlink_suppressed_ &&
             pending_wake_word_.empty())) {
            // A synchronous TTS stop/start callback already advanced the turn.
            return true;
        }
        if (state_after_abort != State::Speaking &&
            !(state_after_abort == State::Idle && transport_.connected())) {
            return false;
        }
        if (!state_machine_.transitionTo(State::Listening)) {
            reportError(ErrorCode::InvalidState, "cannot transition from speaking to listening");
            return false;
        }
        if (end_requested_ || !begun_ || state_machine_.state() != State::Listening) {
            return false;
        }
        setCaptureEnabled(false);
        return enterListeningWhenPlaybackIdleOrClose();
    }
    if (current == State::Listening) {
        setCaptureEnabled(false);
        return enterListeningWhenPlaybackIdleOrClose();
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
        return enterListeningWhenPlaybackIdleOrClose();
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
            return startListening(config_.enable_server_aec &&
                                           config_.enable_voice_barge_in
                                       ? ListeningMode::Realtime
                                       : ListeningMode::AutoStop);
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
    // Local playback must stop on user intent even if the network abort later
    // fails. Waiting for the server leaves already-buffered speech audible.
    downlink_suppressed_ = true;
    cancelPlayback();
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
        return startListening(config_.enable_server_aec &&
                                      config_.enable_voice_barge_in
                                  ? ListeningMode::Realtime
                                  : ListeningMode::AutoStop);
    }
    if (current == State::Speaking) {
        const std::string active_session = session_id_;
        if (!abortSpeaking(AbortReason::WakeWordDetected)) {
            pending_wake_word_.clear();
            return false;
        }
        if (end_requested_ || !begun_ || !session_ready_ ||
            session_id_ != active_session) {
            pending_wake_word_.clear();
            return false;
        }
        const State state_after_abort = state_machine_.state();
        if (state_after_abort == State::Listening ||
            (state_after_abort == State::Speaking && !downlink_suppressed_ &&
             pending_wake_word_.empty())) {
            return true;
        }
        if ((state_after_abort != State::Speaking &&
             !(state_after_abort == State::Idle && transport_.connected())) ||
            !state_machine_.transitionTo(State::Listening)) {
            pending_wake_word_.clear();
            return false;
        }
        setCaptureEnabled(false);
        return enterListeningWhenPlaybackIdleOrClose();
    }
    if (current == State::Listening) {
        downlink_suppressed_ = true;
        cancelPlayback();
        setCaptureEnabled(false);
        std::string abort;
        if (!Protocol::makeAbort(session_id_, AbortReason::WakeWordDetected, abort) ||
            !sendText(abort)) {
            pending_wake_word_.clear();
            return false;
        }
        return enterListeningWhenPlaybackIdleOrClose();
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
    cancelPlayback();
    expected_close_ = true;
    transport_.close();
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
    if (!begun_ || ending_ || !session_ready_ || !capture_enabled_ || !allowed) {
        reportError(ErrorCode::InvalidState,
                    "audio can only be sent while listening or in realtime speaking mode");
        return false;
    }
    if (!transport_.connected()) {
        handleTransportSendFailure("audio transport is disconnected");
        return false;
    }

    // Protocol v1 is the raw Opus payload. Avoid two copies (Client framing and
    // ArduinoWebsockets aggregation) on the default protocol path.
    if (config_.protocol_version == 1) {
        if (opus == nullptr || size == 0 || size > config_.max_audio_payload_bytes) {
            reportError(ErrorCode::InvalidAudioFrame, "Opus payload is empty or too large");
            return false;
        }
        return sendTransportBinary(opus, size);
    }

    std::string error;
    const uint32_t wire_timestamp = config_.enable_server_aec ? timestamp : 0;
    if (!Protocol::encodeAudio(config_.protocol_version, opus, size, wire_timestamp,
                               config_.max_audio_payload_bytes, audio_send_buffer_, error)) {
        reportError(ErrorCode::InvalidAudioFrame, error);
        return false;
    }
    return sendTransportBinary(audio_send_buffer_.data(), audio_send_buffer_.size());
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
    expected_close_ = false;
    std::string hello;
    std::string error;
    if (!Protocol::makeHello(config_, hello, error)) {
        if (end_requested_) {
            return;
        }
        reportError(ErrorCode::InvalidConfiguration, error);
        if (!end_requested_) {
            closeSession();
        }
        return;
    }
    // Publish handshake state before sendText(). A mock or future transport may
    // synchronously deliver the server hello while the send call is still open.
    awaiting_hello_ = true;
    handshake_started_ms_ = clock_.nowMs();
    last_incoming_ms_ = handshake_started_ms_;
    if (!sendText(hello)) {
        awaiting_hello_ = false;
        if (!end_requested_ && state_machine_.state() == State::Connecting) {
            closeSession();
        }
    }
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
                // A stale/competing start can arrive synchronously while a
                // wake abort is still being sent. Until enterListening() moves
                // the pending word into its wire message, keep the old
                // generation suppressed and let the wake flow continue.
                if (!(downlink_suppressed_ && !pending_wake_word_.empty())) {
                    cancelPlayback();
                    downlink_suppressed_ = false;
                    state_machine_.transitionTo(State::Speaking);
                }
            } else if (current == State::Speaking && downlink_suppressed_) {
                // Some servers start the replacement generation without an
                // intermediate stop after acknowledging a local abort.
                if (pending_wake_word_.empty()) {
                    cancelPlayback();
                    downlink_suppressed_ = false;
                }
            }
        } else if (tts_state == "stop") {
            if (state_machine_.state() == State::Speaking) {
                if (listening_mode_ == ListeningMode::ManualStop) {
                    state_machine_.transitionTo(State::Idle);
                } else if (state_machine_.transitionTo(State::Listening)) {
                    if (end_requested_) {
                        return;
                    }
                    enterListeningWhenPlaybackIdleOrClose();
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
        event.emotion_type = emotionFromName(event.emotion);
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
        event.emotion_type = emotionFromName(event.emotion);
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
        std::string response;
        std::string error;
        beginUserCallback();
        const bool handled =
            mcp_server_.handle(payload.as<JsonObjectConst>(), response, error);
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
    AudioFrameView view;
    std::string error;
    if (!Protocol::parseAudioView(config_.protocol_version, data, size,
                                  server_audio_format_,
                                  config_.max_audio_payload_bytes, view, error)) {
        reportError(ErrorCode::InvalidAudioFrame, error);
        return;
    }
    // A valid binary frame proves channel activity even when its audio belongs
    // to an aborted generation. The view lets us gate without allocating Opus.
    last_incoming_ms_ = clock_.nowMs();
    if (downlink_suppressed_ ||
        (!config_.deliver_audio_outside_speaking &&
         state_machine_.state() != State::Speaking)) {
        return;
    }
    AudioFrame frame;
    frame.format = view.format;
    frame.timestamp = view.timestamp;
    frame.opus.assign(view.opus, view.opus + view.opus_size);
    if (callbacks_.on_audio) {
        beginUserCallback();
        callbacks_.on_audio(frame);
        endUserCallback();
        if (end_requested_ || !begun_ || !session_ready_) {
            return;
        }
    }
    if (audio_port_started_ && audio_port_ != nullptr) {
        audio_port_->play(std::move(frame));
    }
}

void Client::onTransportClose() {
    DispatchScope dispatch(*this);
    if (!begun_ || end_requested_) {
        return;
    }
    const bool expected = expected_close_;
    if (expected) {
        return;
    }
    const State state_before_close = state_machine_.state();
    if (state_before_close == State::Idle && !session_ready_ &&
        !awaiting_hello_) {
        return;
    }
    // The official WebSocket protocol treats an established peer close as the
    // normal end of its audio channel. Keep a close during connect/send as an
    // error, but do not leave the UI showing an error after a completed turn.
    const bool report_disconnect = state_before_close == State::Connecting ||
                                   transport_send_in_progress_;
    cancelPlayback();
    // A terminal event does not necessarily mean a custom Transport has
    // discarded every already-queued callback. close() is the explicit barrier.
    expected_close_ = true;
    transport_.close();
    clearSession();
    const State current = state_machine_.state();
    if (current == State::Connecting || current == State::Listening ||
        current == State::Speaking) {
        state_machine_.transitionTo(State::Idle);
    }
    if (!expected && report_disconnect && !end_requested_) {
        reportError(ErrorCode::TransportDisconnected, "WebSocket connection closed");
    }
}

void Client::onTransportError(const std::string& message) {
    DispatchScope dispatch(*this);
    if (expected_close_) {
        return;
    }
    if (begun_ && !end_requested_) {
        const State current = state_machine_.state();
        if (current == State::Idle && !session_ready_ && !awaiting_hello_) {
            return;
        }
        reportError(current == State::Connecting && !session_ready_
                        ? ErrorCode::TransportConnectFailed
                        : ErrorCode::TransportDisconnected,
                    message);
        if (end_requested_) {
            return;
        }
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
    expected_close_ = false;
    deferred_text_chain_count_ = 0;
    ++transport_call_depth_;
    const bool connected = transport_.connect(request);
    --transport_call_depth_;
    if (!connected) {
        // on_open from a transport that ultimately failed may have queued a
        // hello. Never drain protocol work onto a failed connection.
        deferred_text_sends_.clear();
        deferred_text_bytes_ = 0;
        if (end_requested_) {
            return false;
        }
        if (state_machine_.state() == State::Connecting) {
            state_machine_.transitionTo(State::Idle);
            if (end_requested_) {
                return false;
            }
            // A transport that already emitted on_error moved the Client out
            // of Connecting via onTransportError(). Avoid reporting the same
            // failed handshake a second time in that case.
            reportError(ErrorCode::TransportConnectFailed,
                        "WebSocket connect request failed");
        }
        pending_wake_word_.clear();
        return false;
    }
    if (!drainDeferredTextSends()) {
        if (!end_requested_ && begun_ &&
            (state_machine_.state() == State::Connecting || session_ready_)) {
            closeSession();
        }
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

    const std::string active_session = session_id_;
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
        // sendText() may synchronously dispatch a server response. If the
        // server has already advanced this turn to Speaking, do not send a
        // duplicate listen command or let the outer call close a valid session.
        if (!begun_ || !session_ready_ || session_id_ != active_session) {
            return false;
        }
        if (state_machine_.state() == State::Speaking) {
            setCaptureEnabled(listening_mode_ == ListeningMode::Realtime);
            return true;
        }
        if (state_machine_.state() != State::Listening) {
            return false;
        }
    }

    std::string message;
    if (!Protocol::makeStartListening(session_id_, listening_mode_, message) ||
        !sendText(message, DeferredTextRequirement::Listening)) {
        return false;
    }
    if (end_requested_ || !begun_ || !session_ready_ ||
        session_id_ != active_session) {
        return false;
    }
    const State state_after_send = state_machine_.state();
    if (state_after_send == State::Speaking) {
        setCaptureEnabled(listening_mode_ == ListeningMode::Realtime);
        return true;
    }
    if (state_after_send != State::Listening) {
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

bool Client::enterListeningWhenPlaybackIdleOrClose() {
    if (audio_port_started_ && audio_port_ != nullptr &&
        !audio_port_->playbackIdle()) {
        // Keep capture closed until the cancelled/finished I2S write and its
        // short acoustic tail are gone. loop() resumes listening when idle.
        setCaptureEnabled(false);
        pending_listening_start_ = true;
        return true;
    }
    pending_listening_start_ = false;
    return enterListeningOrClose();
}

bool Client::sendText(const std::string& text,
                      DeferredTextRequirement requirement) {
    if (end_requested_) {
        return false;
    }
    if (text.empty() || text.size() > config_.max_json_bytes) {
        reportError(ErrorCode::TransportSendFailed,
                    "text message is empty or exceeds the configured limit");
        return false;
    }
    if (transport_call_depth_ != 0) {
        return queueDeferredText(text, requirement);
    }
    deferred_text_chain_count_ = 0;
    return sendTextNow(text) && drainDeferredTextSends();
}

bool Client::sendTextNow(const std::string& text) {
    if (!transport_.connected()) {
        handleTransportSendFailure("text transport is disconnected");
        return false;
    }
    transport_send_in_progress_ = true;
    ++transport_call_depth_;
    const bool sent =
        transport_.sendText(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    --transport_call_depth_;
    transport_send_in_progress_ = false;
    if (!sent) {
        handleTransportSendFailure("failed to send a text message");
        return false;
    }
    if (!transport_.connected()) {
        handleTransportSendFailure("text transport closed while sending");
        return false;
    }
    return !end_requested_;
}

bool Client::queueDeferredText(const std::string& text,
                               DeferredTextRequirement requirement) {
    const size_t maximum_bytes = config_.max_json_bytes * 2U;
    if (deferred_text_chain_count_ >= kMaximumDeferredTextSends ||
        deferred_text_sends_.size() >= kMaximumDeferredTextSends ||
        text.size() > maximum_bytes - std::min(maximum_bytes, deferred_text_bytes_)) {
        handleTransportSendFailure(
            "synchronous transport callbacks generated too many pending messages");
        return false;
    }
    DeferredTextSend pending;
    pending.text = text;
    pending.session_generation = session_generation_;
    pending.requirement = requirement;
    deferred_text_sends_.push_back(std::move(pending));
    deferred_text_bytes_ += text.size();
    ++deferred_text_chain_count_;
    return true;
}

bool Client::drainDeferredTextSends() {
    while (!deferred_text_sends_.empty() && !end_requested_) {
        DeferredTextSend pending = std::move(deferred_text_sends_.front());
        deferred_text_sends_.erase(deferred_text_sends_.begin());
        deferred_text_bytes_ -= pending.text.size();
        if (pending.session_generation != session_generation_) {
            continue;
        }
        if (pending.requirement == DeferredTextRequirement::Listening &&
            (!session_ready_ || state_machine_.state() != State::Listening)) {
            // A synchronous response has already advanced the turn. This is
            // the queued equivalent of enterListening()'s post-send check.
            if (session_ready_ && state_machine_.state() == State::Speaking) {
                continue;
            }
            deferred_text_sends_.clear();
            deferred_text_bytes_ = 0;
            return false;
        }
        if (!sendTextNow(pending.text)) {
            deferred_text_sends_.clear();
            deferred_text_bytes_ = 0;
            return false;
        }
    }
    if (end_requested_) {
        deferred_text_sends_.clear();
        deferred_text_bytes_ = 0;
        return false;
    }
    return true;
}

bool Client::sendTransportBinary(const uint8_t* data, size_t size) {
    if (!transport_.connected()) {
        handleTransportSendFailure("audio transport is disconnected");
        return false;
    }
    if (transport_call_depth_ != 0) {
        handleTransportSendFailure("audio transport send was recursively re-entered");
        return false;
    }
    deferred_text_chain_count_ = 0;
    transport_send_in_progress_ = true;
    ++transport_call_depth_;
    const bool sent = transport_.sendBinary(data, size);
    --transport_call_depth_;
    transport_send_in_progress_ = false;
    if (!sent) {
        handleTransportSendFailure("failed to send an audio frame");
        return false;
    }
    if (!transport_.connected()) {
        handleTransportSendFailure("audio transport closed while sending");
        return false;
    }
    return !end_requested_ && drainDeferredTextSends();
}

void Client::handleTransportSendFailure(const char* message) {
    if (!begun_ || ending_ || end_requested_) {
        return;
    }
    const State current = state_machine_.state();
    const bool active = current == State::Connecting || current == State::Listening ||
                        current == State::Speaking || session_ready_ || awaiting_hello_;
    // A synchronous on_close/on_error may already have reported and cleaned up
    // the connection before send*() returns false.
    if (!active) {
        return;
    }
    reportError(ErrorCode::TransportSendFailed, message);
    if (!end_requested_ && begun_ && !ending_) {
        closeSession();
    }
}

void Client::clearSession() {
    ++session_generation_;
    awaiting_hello_ = false;
    session_ready_ = false;
    handshake_started_ms_ = 0;
    last_incoming_ms_ = 0;
    session_id_.clear();
    pending_wake_word_.clear();
    pending_listening_start_ = false;
    downlink_suppressed_ = false;
    deferred_text_sends_.clear();
    deferred_text_bytes_ = 0;
    server_audio_format_ = AudioFormat{24000, 60, 1};
}

void Client::cancelPlayback() {
    if (audio_port_started_ && audio_port_ != nullptr) {
        audio_port_->cancelPlayback();
    }
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
