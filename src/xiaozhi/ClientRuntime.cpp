#include "ClientRuntime.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace xiaozhi {
namespace {

TickType_t timeoutTicks(uint32_t timeout_ms) {
    if (timeout_ms == std::numeric_limits<uint32_t>::max()) {
        return portMAX_DELAY;
    }
    if (timeout_ms == 0) {
        return 0;
    }
    return std::max<TickType_t>(1, pdMS_TO_TICKS(timeout_ms));
}

template <typename T>
void updateHighWatermark(std::atomic<uint8_t>& maximum, T value) {
    const uint8_t bounded = static_cast<uint8_t>(
        std::min<T>(value, std::numeric_limits<uint8_t>::max()));
    uint8_t previous = maximum.load(std::memory_order_relaxed);
    while (bounded > previous &&
           !maximum.compare_exchange_weak(previous, bounded,
                                          std::memory_order_relaxed)) {
    }
}

void updateMaximum(std::atomic<uint32_t>& maximum, uint32_t value) {
    uint32_t previous = maximum.load(std::memory_order_relaxed);
    while (value > previous &&
           !maximum.compare_exchange_weak(previous, value,
                                          std::memory_order_relaxed)) {
    }
}

uint32_t runtimeNowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

}  // namespace

class ClientRuntime::Impl final : public RealtimeControlSink {
public:
    explicit Impl(Client& client) : client_(client), state_(client.state()) {}

    ~Impl() {
        end(std::numeric_limits<uint32_t>::max());
        releaseResources();
    }

    bool begin(const ClientConfig& client_config, Callbacks callbacks,
               const ClientRuntimeConfig& runtime_config) {
        if (task_active_.load() || running_.load() ||
            !validConfig(runtime_config)) {
            return false;
        }

        releaseResources();
        runtime_config_ = runtime_config;
        client_config_ = client_config;
        user_callbacks_ = std::move(callbacks);
        stop_requested_.store(false);
        startup_succeeded_.store(false);
        resetStats();

        command_queue_ = xQueueCreate(runtime_config_.command_queue_depth,
                                      sizeof(Command*));
        callback_queue_ = xQueueCreate(runtime_config_.callback_queue_depth,
                                       sizeof(CallbackMessage*));
        wake_event_queue_ = xQueueCreate(1, sizeof(WakeEvent));
        startup_signal_ = xSemaphoreCreateBinary();
        stopped_signal_ = xSemaphoreCreateBinary();
        if (command_queue_ == nullptr || callback_queue_ == nullptr ||
            wake_event_queue_ == nullptr || startup_signal_ == nullptr ||
            stopped_signal_ == nullptr) {
            releaseResources();
            return false;
        }

        task_active_.store(true);
        BaseType_t created = pdFAIL;
        if (runtime_config_.task_core < 0) {
            created = xTaskCreate(serviceTaskEntry, "xiaozhi", runtime_config_.task_stack_size,
                                  this, runtime_config_.task_priority, nullptr);
        } else {
            created = xTaskCreatePinnedToCore(
                serviceTaskEntry, "xiaozhi", runtime_config_.task_stack_size, this,
                runtime_config_.task_priority, nullptr, runtime_config_.task_core);
        }
        if (created != pdPASS) {
            task_active_.store(false);
            releaseResources();
            return false;
        }

        xSemaphoreTake(startup_signal_, portMAX_DELAY);

        const bool succeeded = startup_succeeded_.load();
        if (!succeeded) {
            xSemaphoreTake(stopped_signal_, portMAX_DELAY);
        }
        return succeeded;
    }

    bool end(uint32_t timeout_ms) {
        if (!task_active_.load() && !running_.load()) {
            return true;
        }
        stop_requested_.store(true);
        if (stopped_signal_ == nullptr ||
            xSemaphoreTake(stopped_signal_, timeoutTicks(timeout_ms)) != pdTRUE) {
            return false;
        }
        return true;
    }

    size_t loop(size_t max_callbacks) {
        if (callback_queue_ == nullptr) {
            return 0;
        }
        size_t dispatched = 0;
        CallbackMessage* message = nullptr;
        while ((max_callbacks == 0 || dispatched < max_callbacks) &&
               xQueueReceive(callback_queue_, &message, 0) == pdTRUE) {
            if (message != nullptr) {
                dispatchCallback(*message);
                delete message;
                ++callbacks_dispatched_;
            }
            ++dispatched;
            if (callback_queue_ == nullptr) {
                break;
            }
        }
        return dispatched;
    }

    bool requestStartListening(ListeningMode mode) {
        Command* command = allocateCommand(CommandType::StartListening);
        if (command != nullptr) {
            command->listening_mode = mode;
        }
        return enqueueCommand(command);
    }

    bool requestStopListening() {
        return enqueueCommand(allocateCommand(CommandType::StopListening));
    }

    bool requestToggleChat() {
        return enqueueCommand(allocateCommand(CommandType::ToggleChat));
    }

    bool requestAbortSpeaking(AbortReason reason) {
        Command* command = allocateCommand(CommandType::AbortSpeaking);
        if (command != nullptr) {
            command->abort_reason = reason;
        }
        return enqueueCommand(command);
    }

    bool requestWakeWordDetected(const std::string& wake_word) {
        Command* command = allocateCommand(CommandType::WakeWordDetected);
        if (command != nullptr) {
            command->payload = wake_word;
        }
        return enqueueCommand(command);
    }

    bool requestCloseSession() {
        return enqueueCommand(allocateCommand(CommandType::CloseSession));
    }

    bool requestSendMcp(const std::string& payload) {
        Command* command = allocateCommand(CommandType::SendMcp);
        if (command != nullptr) {
            command->payload = payload;
        }
        return enqueueCommand(command);
    }

    bool notifyWakeWordDetected(const char* wake_word, size_t size,
                                bool from_isr) override {
        if (wake_word == nullptr || size == 0 ||
            size > RealtimeControlSink::kMaximumWakeWordBytes ||
            wake_event_queue_ == nullptr || !running_.load() ||
            stop_requested_.load()) {
            ++wake_events_rejected_;
            return false;
        }
        WakeEvent event;
        event.size = static_cast<uint8_t>(size);
        std::copy_n(wake_word, size, event.text);
        event.text[size] = '\0';
        BaseType_t queued = pdFALSE;
        if (from_isr) {
            BaseType_t higher_priority_task_woken = pdFALSE;
            queued = xQueueSendFromISR(wake_event_queue_, &event,
                                       &higher_priority_task_woken);
            if (higher_priority_task_woken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
        } else {
            queued = xQueueSend(wake_event_queue_, &event, 0);
        }
        if (queued != pdTRUE) {
            // One pending wake is sufficient: WakeNet disables its generation
            // after detection, and duplicate producers must not queue storms.
            ++wake_events_coalesced_;
            return true;
        }
        ++wake_events_queued_;
        return true;
    }

    bool running() const { return running_.load(); }
    bool ready() const { return ready_.load(); }
    bool sessionReady() const { return session_ready_.load(); }
    State state() const { return state_.load(); }

    ClientRuntimeStats stats() const {
        ClientRuntimeStats output;
        output.service_cycles = service_cycles_.load();
        output.commands_queued = commands_queued_.load();
        output.commands_executed = commands_executed_.load();
        output.commands_rejected = commands_rejected_.load();
        output.callbacks_queued = callbacks_queued_.load();
        output.callbacks_dispatched = callbacks_dispatched_.load();
        output.callbacks_dropped = callbacks_dropped_.load();
        output.wake_events_queued = wake_events_queued_.load();
        output.wake_events_executed = wake_events_executed_.load();
        output.wake_events_coalesced = wake_events_coalesced_.load();
        output.wake_events_rejected = wake_events_rejected_.load();
        output.lifecycle_hook_calls = lifecycle_hook_calls_.load();
        output.lifecycle_hook_overruns = lifecycle_hook_overruns_.load();
        output.lifecycle_hook_maximum_us = lifecycle_hook_maximum_us_.load();
        output.playback_first_audio_timeouts =
            playback_first_audio_timeouts_.load();
        output.playback_inter_packet_timeouts =
            playback_inter_packet_timeouts_.load();
        output.playback_timeout_close_failures =
            playback_timeout_close_failures_.load();
        output.playback_idle = playback_idle_.load();
        output.command_queue_high_watermark = command_queue_high_watermark_.load();
        output.callback_queue_high_watermark = callback_queue_high_watermark_.load();
        return output;
    }

private:
    enum class CommandType : uint8_t {
        StartListening,
        StopListening,
        ToggleChat,
        AbortSpeaking,
        WakeWordDetected,
        CloseSession,
        SendMcp,
    };

    struct Command {
        explicit Command(CommandType value) : type(value) {}
        CommandType type;
        ListeningMode listening_mode = ListeningMode::ManualStop;
        AbortReason abort_reason = AbortReason::None;
        std::string payload;
    };

    struct WakeEvent {
        uint8_t size = 0;
        char text[RealtimeControlSink::kMaximumWakeWordBytes + 1]{};
    };

    enum class CallbackType : uint8_t {
        StateChanged,
        WakeWord,
        Event,
        Audio,
        Capture,
        Error,
    };

    struct CallbackMessage {
        explicit CallbackMessage(CallbackType value) : type(value) {}
        CallbackType type;
        State old_state = State::Unknown;
        State new_state = State::Unknown;
        std::string wake_word;
        Event event;
        AudioFrame audio;
        bool capture_enabled = false;
        AudioFormat capture_format;
        ErrorCode error_code = ErrorCode::None;
        std::string error_message;
    };

    Client& client_;
    ClientConfig client_config_;
    ClientRuntimeConfig runtime_config_;
    Callbacks user_callbacks_;
    QueueHandle_t command_queue_ = nullptr;
    QueueHandle_t callback_queue_ = nullptr;
    QueueHandle_t wake_event_queue_ = nullptr;
    SemaphoreHandle_t startup_signal_ = nullptr;
    SemaphoreHandle_t stopped_signal_ = nullptr;
    std::atomic<bool> task_active_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> session_ready_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> startup_succeeded_{false};
    std::atomic<State> state_{State::Unknown};
    std::atomic<uint32_t> service_cycles_{0};
    std::atomic<uint32_t> commands_queued_{0};
    std::atomic<uint32_t> commands_executed_{0};
    std::atomic<uint32_t> commands_rejected_{0};
    std::atomic<uint32_t> callbacks_queued_{0};
    std::atomic<uint32_t> callbacks_dispatched_{0};
    std::atomic<uint32_t> callbacks_dropped_{0};
    std::atomic<uint32_t> wake_events_queued_{0};
    std::atomic<uint32_t> wake_events_executed_{0};
    std::atomic<uint32_t> wake_events_coalesced_{0};
    std::atomic<uint32_t> wake_events_rejected_{0};
    std::atomic<uint32_t> lifecycle_hook_calls_{0};
    std::atomic<uint32_t> lifecycle_hook_overruns_{0};
    std::atomic<uint32_t> lifecycle_hook_maximum_us_{0};
    std::atomic<uint32_t> playback_first_audio_timeouts_{0};
    std::atomic<uint32_t> playback_inter_packet_timeouts_{0};
    std::atomic<uint32_t> playback_timeout_close_failures_{0};
    std::atomic<bool> playback_idle_{true};
    std::atomic<uint8_t> command_queue_high_watermark_{0};
    std::atomic<uint8_t> callback_queue_high_watermark_{0};

    // Owned exclusively by the Runtime service task. uint32_t subtraction is
    // intentionally wrap-safe across the Arduino millisecond counter rollover.
    uint32_t speaking_started_ms_ = 0;
    uint32_t first_downlink_audio_ms_ = 0;
    uint32_t last_downlink_audio_ms_ = 0;
    bool playback_watchdog_armed_ = false;
    bool received_downlink_audio_ = false;

    static void serviceTaskEntry(void* argument) {
        static_cast<Impl*>(argument)->serviceTask();
    }

    bool validConfig(const ClientRuntimeConfig& config) const {
        return config.task_stack_size >= 4096 && config.task_priority > 0 &&
               config.task_priority < configMAX_PRIORITIES &&
               config.task_core >= -1 && config.task_core < portNUM_PROCESSORS &&
               config.poll_interval_ms > 0 && config.poll_interval_ms <= 1000 &&
               config.idle_poll_interval_ms > 0 &&
               config.idle_poll_interval_ms <= 1000 &&
               config.command_queue_depth > 0 && config.callback_queue_depth > 0 &&
               config.maximum_commands_per_cycle > 0 &&
               (!config.playback_watchdog.enabled ||
                (config.playback_watchdog.first_audio_timeout_ms > 0 &&
                 config.playback_watchdog.first_audio_timeout_ms <= 3600000 &&
                 config.playback_watchdog.inter_packet_timeout_ms > 0 &&
                 config.playback_watchdog.inter_packet_timeout_ms <= 3600000)) &&
               (config.lifecycle.on_state_changed == nullptr ||
                (config.lifecycle.maximum_execution_us > 0 &&
                 config.lifecycle.maximum_execution_us <= 1000000));
    }

    Callbacks makeDeferredCallbacks() {
        Callbacks callbacks;
        if (user_callbacks_.on_state_changed ||
            runtime_config_.playback_watchdog.enabled ||
            runtime_config_.lifecycle.on_state_changed != nullptr) {
            callbacks.on_state_changed = [this](State old_state, State new_state) {
                updatePlaybackState(old_state, new_state);
                runLifecycleHook(old_state, new_state);
                // Publish the new state only after its internal lifecycle work
                // has completed, so readers cannot observe a half-applied state.
                state_.store(new_state);
                if (user_callbacks_.on_state_changed) {
                    CallbackMessage* message =
                        allocateCallback(CallbackType::StateChanged);
                    if (message != nullptr) {
                        message->old_state = old_state;
                        message->new_state = new_state;
                    }
                    enqueueCallback(message);
                }
            };
        }
        if (user_callbacks_.on_event) {
            callbacks.on_event = [this](const Event& event) {
                CallbackMessage* message = allocateCallback(CallbackType::Event);
                if (message != nullptr) {
                    message->event = event;
                }
                enqueueCallback(message);
            };
        }
        if (user_callbacks_.on_audio || runtime_config_.playback_watchdog.enabled) {
            callbacks.on_audio = [this](const AudioFrame& audio) {
                noteDownlinkAudio();
                if (!user_callbacks_.on_audio) {
                    return;
                }
                // Audio callbacks are observers; attached EncodedAudioPort
                // playback is delivered directly by Client. Avoid allocating
                // and copying Opus when a stalled user loop has filled its
                // best-effort callback queue.
                if (callback_queue_ == nullptr ||
                    uxQueueSpacesAvailable(callback_queue_) == 0) {
                    ++callbacks_dropped_;
                    return;
                }
                CallbackMessage* message = allocateCallback(CallbackType::Audio);
                if (message != nullptr) {
                    message->audio = audio;
                }
                enqueueCallback(message);
            };
        }
        if (user_callbacks_.on_capture) {
            callbacks.on_capture = [this](bool enabled, const AudioFormat& format) {
                CallbackMessage* message = allocateCallback(CallbackType::Capture);
                if (message != nullptr) {
                    message->capture_enabled = enabled;
                    message->capture_format = format;
                }
                enqueueCallback(message);
            };
        }
        if (user_callbacks_.on_error) {
            callbacks.on_error = [this](ErrorCode code, const std::string& error) {
                CallbackMessage* message = allocateCallback(CallbackType::Error);
                if (message != nullptr) {
                    message->error_code = code;
                    message->error_message = error;
                }
                enqueueCallback(message);
            };
        }
        return callbacks;
    }

    void runLifecycleHook(State old_state, State new_state) {
        const ClientRuntimeLifecycleHooks& lifecycle = runtime_config_.lifecycle;
        if (lifecycle.on_state_changed == nullptr) {
            return;
        }
        const int64_t started_us = esp_timer_get_time();
        lifecycle.on_state_changed(lifecycle.context, old_state, new_state,
                                   client_.sessionReady());
        const int64_t elapsed_us = std::max<int64_t>(0, esp_timer_get_time() - started_us);
        const uint32_t bounded_us = static_cast<uint32_t>(
            std::min<int64_t>(elapsed_us, std::numeric_limits<uint32_t>::max()));
        ++lifecycle_hook_calls_;
        updateMaximum(lifecycle_hook_maximum_us_, bounded_us);
        if (bounded_us > lifecycle.maximum_execution_us) {
            ++lifecycle_hook_overruns_;
        }
    }

    void updatePlaybackState(State, State new_state) {
        if (!runtime_config_.playback_watchdog.enabled) {
            return;
        }
        if (new_state == State::Speaking) {
            speaking_started_ms_ = runtimeNowMs();
            first_downlink_audio_ms_ = 0;
            last_downlink_audio_ms_ = 0;
            received_downlink_audio_ = false;
            playback_watchdog_armed_ = true;
            refreshPlaybackSnapshot();
            return;
        }
        playback_watchdog_armed_ = false;
        received_downlink_audio_ = false;
        speaking_started_ms_ = 0;
        first_downlink_audio_ms_ = 0;
        last_downlink_audio_ms_ = 0;
        refreshPlaybackSnapshot();
    }

    void noteDownlinkAudio() {
        if (!runtime_config_.playback_watchdog.enabled ||
            client_.state() != State::Speaking || !playback_watchdog_armed_) {
            return;
        }
        const uint32_t now_ms = runtimeNowMs();
        if (!received_downlink_audio_) {
            first_downlink_audio_ms_ = now_ms;
            received_downlink_audio_ = true;
        }
        last_downlink_audio_ms_ = now_ms;
    }

    void refreshPlaybackSnapshot() {
        playback_idle_.store(client_.playbackIdle());
    }

    void servicePlaybackWatchdog() {
        if (!runtime_config_.playback_watchdog.enabled ||
            !playback_watchdog_armed_ || client_.state() != State::Speaking) {
            return;
        }
        const uint32_t now_ms = runtimeNowMs();
        const uint32_t reference_ms = received_downlink_audio_
                                          ? last_downlink_audio_ms_
                                          : speaking_started_ms_;
        const uint32_t timeout_ms =
            received_downlink_audio_
                ? runtime_config_.playback_watchdog.inter_packet_timeout_ms
                : runtime_config_.playback_watchdog.first_audio_timeout_ms;
        if (now_ms - reference_ms < timeout_ms) {
            return;
        }
        // Sampling is deferred until the deadline so the normal 2 ms service
        // loop does not contend with audio workers on every cycle.
        refreshPlaybackSnapshot();
        if (!playback_idle_.load()) {
            return;
        }

        // Disarm before acting because closeSession() synchronously emits the
        // state result. This owner-task path runs ahead of ordinary commands.
        playback_watchdog_armed_ = false;
        if (received_downlink_audio_) {
            ++playback_inter_packet_timeouts_;
        } else {
            ++playback_first_audio_timeouts_;
        }
        if (!client_.closeSession()) {
            ++playback_timeout_close_failures_;
        }
        snapshotClient();
    }

    void serviceTask() {
        const bool sink_attached = client_.attachRealtimeControlSink(this);
        const bool begun = sink_attached &&
                           client_.begin(client_config_, makeDeferredCallbacks());
        startup_succeeded_.store(begun);
        running_.store(begun);
        snapshotClient();
        xSemaphoreGive(startup_signal_);

        if (begun) {
            while (!stop_requested_.load()) {
                servicePlaybackWatchdog();
                processWakeEvents();
                processCommands();
                if (stop_requested_.load()) {
                    break;
                }
                client_.loop();
                snapshotClient();
                ++service_cycles_;
                const bool active = client_.sessionReady() ||
                                    client_.state() == State::Connecting;
                const uint16_t interval_ms =
                    active ? runtime_config_.poll_interval_ms
                           : runtime_config_.idle_poll_interval_ms;
                const TickType_t poll_ticks =
                    std::max<TickType_t>(1, pdMS_TO_TICKS(interval_ms));
                Command* command = nullptr;
                if (xQueueReceive(command_queue_, &command, poll_ticks) == pdTRUE) {
                    executeCommand(command);
                    snapshotClient();
                }
            }
            discardCommands();
            client_.end();
        }

        if (sink_attached) {
            client_.attachRealtimeControlSink(nullptr);
        }

        snapshotClient();
        running_.store(false);
        task_active_.store(false);
        xSemaphoreGive(stopped_signal_);
        vTaskDelete(nullptr);
    }

    void processCommands() {
        for (uint8_t index = 0; index < runtime_config_.maximum_commands_per_cycle;
             ++index) {
            Command* command = nullptr;
            if (xQueueReceive(command_queue_, &command, 0) != pdTRUE) {
                return;
            }
            if (command != nullptr) {
                executeCommand(command);
            }
            snapshotClient();
            if (stop_requested_.load()) {
                return;
            }
        }
    }

    void processWakeEvents() {
        if (wake_event_queue_ == nullptr) {
            return;
        }
        WakeEvent event;
        if (xQueueReceive(wake_event_queue_, &event, 0) != pdTRUE) {
            return;
        }
        const State current = client_.state();
        if (!client_.ready() ||
            (current != State::Idle && current != State::Listening &&
             current != State::Speaking)) {
            // Connecting and other transient states already represent a wake
            // in progress; consume duplicates without generating Client errors.
            ++wake_events_rejected_;
            return;
        }
        std::string wake_word(event.text, event.size);
        if (!client_.wakeWordDetected(wake_word)) {
            ++wake_events_rejected_;
            return;
        }
        ++wake_events_executed_;
        enqueueWakeObserver(std::move(wake_word));
    }

    Command* allocateCommand(CommandType type) {
        if (!running_.load() || stop_requested_.load()) {
            return nullptr;
        }
        return new (std::nothrow) Command(type);
    }

    bool enqueueCommand(Command* command) {
        if (command == nullptr || command_queue_ == nullptr || !running_.load() ||
            stop_requested_.load()) {
            delete command;
            ++commands_rejected_;
            return false;
        }
        if (xQueueSend(command_queue_, &command, 0) != pdTRUE) {
            delete command;
            ++commands_rejected_;
            return false;
        }
        ++commands_queued_;
        updateHighWatermark(command_queue_high_watermark_,
                            uxQueueMessagesWaiting(command_queue_));
        return true;
    }

    CallbackMessage* allocateCallback(CallbackType type) {
        CallbackMessage* message = new (std::nothrow) CallbackMessage(type);
        if (message == nullptr) {
            ++callbacks_dropped_;
        }
        return message;
    }

    void enqueueCallback(CallbackMessage* message) {
        if (message == nullptr) {
            return;
        }
        if (callback_queue_ == nullptr) {
            delete message;
            ++callbacks_dropped_;
            return;
        }
        if (xQueueSend(callback_queue_, &message, 0) != pdTRUE) {
            if (message->type == CallbackType::Audio) {
                delete message;
                ++callbacks_dropped_;
                return;
            }
            // State/event/error delivery has priority over a stale observer
            // backlog. Evict one oldest callback, never wait on user code.
            CallbackMessage* oldest = nullptr;
            if (xQueueReceive(callback_queue_, &oldest, 0) == pdTRUE) {
                delete oldest;
                ++callbacks_dropped_;
            }
            if (xQueueSend(callback_queue_, &message, 0) != pdTRUE) {
                delete message;
                ++callbacks_dropped_;
                return;
            }
        }
        ++callbacks_queued_;
        updateHighWatermark(callback_queue_high_watermark_,
                            uxQueueMessagesWaiting(callback_queue_));
    }

    void enqueueWakeObserver(std::string wake_word) {
        if (!user_callbacks_.on_wake_word) {
            return;
        }
        CallbackMessage* message = allocateCallback(CallbackType::WakeWord);
        if (message != nullptr) {
            message->wake_word = std::move(wake_word);
        }
        enqueueCallback(message);
    }

    void dispatchCallback(const CallbackMessage& message) {
        switch (message.type) {
            case CallbackType::StateChanged:
                if (user_callbacks_.on_state_changed) {
                    user_callbacks_.on_state_changed(message.old_state, message.new_state);
                }
                break;
            case CallbackType::WakeWord:
                if (user_callbacks_.on_wake_word) {
                    user_callbacks_.on_wake_word(message.wake_word);
                }
                break;
            case CallbackType::Event:
                if (user_callbacks_.on_event) {
                    user_callbacks_.on_event(message.event);
                }
                break;
            case CallbackType::Audio:
                if (user_callbacks_.on_audio) {
                    user_callbacks_.on_audio(message.audio);
                }
                break;
            case CallbackType::Capture:
                if (user_callbacks_.on_capture) {
                    user_callbacks_.on_capture(message.capture_enabled,
                                               message.capture_format);
                }
                break;
            case CallbackType::Error:
                if (user_callbacks_.on_error) {
                    user_callbacks_.on_error(message.error_code, message.error_message);
                }
                break;
        }
    }

    void snapshotClient() {
        ready_.store(client_.ready());
        session_ready_.store(client_.sessionReady());
        state_.store(client_.state());
    }

    void executeCommand(Command* command) {
        if (command == nullptr) {
            return;
        }
        bool accepted = false;
        switch (command->type) {
            case CommandType::StartListening:
                accepted = client_.startListening(command->listening_mode);
                break;
            case CommandType::StopListening:
                accepted = client_.stopListening();
                break;
            case CommandType::ToggleChat:
                accepted = client_.toggleChat();
                break;
            case CommandType::AbortSpeaking:
                accepted = client_.abortSpeaking(command->abort_reason);
                break;
            case CommandType::WakeWordDetected:
                accepted = client_.wakeWordDetected(command->payload);
                if (accepted) {
                    enqueueWakeObserver(command->payload);
                }
                break;
            case CommandType::CloseSession:
                accepted = client_.closeSession();
                break;
            case CommandType::SendMcp:
                accepted = client_.sendMcp(command->payload);
                break;
        }
        ++commands_executed_;
        if (!accepted) {
            ++commands_rejected_;
        }
        delete command;
    }

    void discardCommands() {
        if (command_queue_ == nullptr) {
            return;
        }
        Command* command = nullptr;
        while (xQueueReceive(command_queue_, &command, 0) == pdTRUE) {
            delete command;
        }
    }

    void discardCallbacks() {
        if (callback_queue_ == nullptr) {
            return;
        }
        CallbackMessage* message = nullptr;
        while (xQueueReceive(callback_queue_, &message, 0) == pdTRUE) {
            delete message;
        }
    }

    void releaseResources() {
        if (task_active_.load() || running_.load()) {
            return;
        }
        discardCommands();
        discardCallbacks();
        if (command_queue_ != nullptr) {
            vQueueDelete(command_queue_);
            command_queue_ = nullptr;
        }
        if (callback_queue_ != nullptr) {
            vQueueDelete(callback_queue_);
            callback_queue_ = nullptr;
        }
        if (wake_event_queue_ != nullptr) {
            vQueueDelete(wake_event_queue_);
            wake_event_queue_ = nullptr;
        }
        if (startup_signal_ != nullptr) {
            vSemaphoreDelete(startup_signal_);
            startup_signal_ = nullptr;
        }
        if (stopped_signal_ != nullptr) {
            vSemaphoreDelete(stopped_signal_);
            stopped_signal_ = nullptr;
        }
        ready_.store(false);
        session_ready_.store(false);
    }

    void resetStats() {
        service_cycles_.store(0);
        commands_queued_.store(0);
        commands_executed_.store(0);
        commands_rejected_.store(0);
        callbacks_queued_.store(0);
        callbacks_dispatched_.store(0);
        callbacks_dropped_.store(0);
        wake_events_queued_.store(0);
        wake_events_executed_.store(0);
        wake_events_coalesced_.store(0);
        wake_events_rejected_.store(0);
        lifecycle_hook_calls_.store(0);
        lifecycle_hook_overruns_.store(0);
        lifecycle_hook_maximum_us_.store(0);
        playback_first_audio_timeouts_.store(0);
        playback_inter_packet_timeouts_.store(0);
        playback_timeout_close_failures_.store(0);
        playback_idle_.store(true);
        speaking_started_ms_ = 0;
        first_downlink_audio_ms_ = 0;
        last_downlink_audio_ms_ = 0;
        playback_watchdog_armed_ = false;
        received_downlink_audio_ = false;
        command_queue_high_watermark_.store(0);
        callback_queue_high_watermark_.store(0);
    }
};

ClientRuntime::ClientRuntime(Client& client) : impl_(new Impl(client)) {}

ClientRuntime::~ClientRuntime() = default;

bool ClientRuntime::begin(const ClientConfig& client_config, Callbacks callbacks,
                          const ClientRuntimeConfig& runtime_config) {
    return impl_->begin(client_config, std::move(callbacks), runtime_config);
}

bool ClientRuntime::end(uint32_t timeout_ms) { return impl_->end(timeout_ms); }

size_t ClientRuntime::loop(size_t max_callbacks) {
    return impl_->loop(max_callbacks);
}

bool ClientRuntime::requestStartListening(ListeningMode mode) {
    return impl_->requestStartListening(mode);
}

bool ClientRuntime::requestStopListening() { return impl_->requestStopListening(); }

bool ClientRuntime::requestToggleChat() { return impl_->requestToggleChat(); }

bool ClientRuntime::requestAbortSpeaking(AbortReason reason) {
    return impl_->requestAbortSpeaking(reason);
}

bool ClientRuntime::requestWakeWordDetected(const std::string& wake_word) {
    return impl_->requestWakeWordDetected(wake_word);
}

bool ClientRuntime::requestCloseSession() { return impl_->requestCloseSession(); }

bool ClientRuntime::requestSendMcp(const std::string& json_rpc_payload) {
    return impl_->requestSendMcp(json_rpc_payload);
}

bool ClientRuntime::running() const { return impl_->running(); }

bool ClientRuntime::ready() const { return impl_->ready(); }

bool ClientRuntime::sessionReady() const { return impl_->sessionReady(); }

State ClientRuntime::state() const { return impl_->state(); }

ClientRuntimeStats ClientRuntime::stats() const { return impl_->stats(); }

}  // namespace xiaozhi
