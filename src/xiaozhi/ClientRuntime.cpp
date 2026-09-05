#include "ClientRuntime.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
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

template <typename T>
class FixedSlotPool {
public:
    bool initialize(uint8_t capacity) {
        reset();
        if (capacity == 0 || capacity > 32) {
            return false;
        }
        slots_.reset(new (std::nothrow) T[capacity]);
        if (!slots_) {
            return false;
        }
        capacity_ = capacity;
        free_mask_.store(capacity == 32 ? std::numeric_limits<uint32_t>::max()
                                        : ((1U << capacity) - 1U));
        return true;
    }

    T* acquire(uint8_t& index) {
        uint32_t available = free_mask_.load(std::memory_order_relaxed);
        while (available != 0) {
            uint8_t candidate = 0;
            while ((available & (1U << candidate)) == 0) {
                ++candidate;
            }
            const uint32_t desired = available & ~(1U << candidate);
            if (free_mask_.compare_exchange_weak(
                    available, desired, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                index = candidate;
                return &slots_[candidate];
            }
        }
        return nullptr;
    }

    T& at(uint8_t index) { return slots_[index]; }
    const T& at(uint8_t index) const { return slots_[index]; }

    void release(uint8_t index) {
        if (index >= capacity_) {
            return;
        }
        slots_[index].clear();
        free_mask_.fetch_or(1U << index, std::memory_order_release);
    }

    void reset() {
        free_mask_.store(0);
        slots_.reset();
        capacity_ = 0;
    }

private:
    std::unique_ptr<T[]> slots_;
    std::atomic<uint32_t> free_mask_{0};
    uint8_t capacity_ = 0;
};

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
        if (callback_dispatch_depth_.load() != 0 || task_active_.load() ||
            running_.load() ||
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
                                      sizeof(Command));
        callback_queue_ = xQueueCreate(runtime_config_.callback_queue_depth,
                                       sizeof(CallbackMessage));
        state_callback_queue_ = xQueueCreate(1, sizeof(CallbackMessage));
        capture_callback_queue_ = xQueueCreate(1, sizeof(CallbackMessage));
        audio_callback_queue_ = xQueueCreate(
            runtime_config_.audio_callback_queue_depth,
            sizeof(CallbackMessage));
        wake_event_queue_ = xQueueCreate(1, sizeof(WakeEvent));
        startup_signal_ = xSemaphoreCreateBinary();
        stopped_signal_ = xSemaphoreCreateBinary();
        if (command_queue_ == nullptr || callback_queue_ == nullptr ||
            state_callback_queue_ == nullptr ||
            capture_callback_queue_ == nullptr ||
            audio_callback_queue_ == nullptr ||
            wake_event_queue_ == nullptr || startup_signal_ == nullptr ||
            stopped_signal_ == nullptr || !initializePayloadPools()) {
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
            task_active_.store(false);
            releaseResources();
        }
        return succeeded;
    }

    bool end(uint32_t timeout_ms) {
        // A callback payload is a borrowed pool slot until the callback
        // returns. Reject reentrant teardown so loop() can release it safely.
        if (callback_dispatch_depth_.load() != 0) {
            return false;
        }
        if (!task_active_.load() && !running_.load()) {
            releaseResources();
            return true;
        }
        stop_requested_.store(true);
        notifyServiceTask(false);
        if (stopped_signal_ == nullptr ||
            xSemaphoreTake(stopped_signal_, timeoutTicks(timeout_ms)) != pdTRUE) {
            return false;
        }
        task_active_.store(false);
        releaseResources();
        return true;
    }

    size_t loop(size_t max_callbacks) {
        if (callback_queue_ == nullptr) {
            return 0;
        }
        size_t dispatched = 0;
        CallbackMessage message;
        while (max_callbacks == 0 || dispatched < max_callbacks) {
            if (!receiveNextCallback(message)) {
                break;
            }
            callback_dispatch_depth_.fetch_add(1);
            dispatchCallback(message);
            releaseCallbackPayload(message);
            callback_dispatch_depth_.fetch_sub(1);
            ++callbacks_dispatched_;
            ++dispatched;
            if (callback_queue_ == nullptr || state_callback_queue_ == nullptr ||
                capture_callback_queue_ == nullptr ||
                audio_callback_queue_ == nullptr) {
                break;
            }
        }
        return dispatched;
    }

    bool requestStartListening(ListeningMode mode) {
        Command command;
        command.type = CommandType::StartListening;
        command.listening_mode = mode;
        return enqueueCommand(command);
    }

    bool requestStopListening() {
        return publishUrgentControl(kUrgentStopListening, false);
    }

    bool requestToggleChat() {
        Command command;
        command.type = CommandType::ToggleChat;
        return enqueueCommand(command);
    }

    bool requestAbortSpeaking(AbortReason reason) {
        pending_abort_reason_.store(static_cast<uint8_t>(reason));
        return publishUrgentControl(kUrgentAbortSpeaking, false);
    }

    bool requestWakeWordDetected(const std::string& wake_word) {
        return notifyWakeWordDetected(wake_word.data(), wake_word.size(), false);
    }

    bool requestWakeWordDetected(const char* wake_word, size_t size) {
        return notifyWakeWordDetected(wake_word, size, false);
    }

    bool requestPlaybackMute(bool muted) {
        pending_playback_mute_.store(muted);
        return publishUrgentControl(kUrgentPlaybackMute, false);
    }

    bool requestStopListeningFromISR() {
        return publishUrgentControl(kUrgentStopListening, true);
    }

    bool requestAbortSpeakingFromISR(AbortReason reason) {
        pending_abort_reason_.store(static_cast<uint8_t>(reason));
        return publishUrgentControl(kUrgentAbortSpeaking, true);
    }

    bool requestWakeWordDetectedFromISR(const char* wake_word, size_t size) {
        return notifyWakeWordDetected(wake_word, size, true);
    }

    bool requestPlaybackMuteFromISR(bool muted) {
        pending_playback_mute_.store(muted);
        return publishUrgentControl(kUrgentPlaybackMute, true);
    }

    bool requestCloseSession() {
        Command command;
        command.type = CommandType::CloseSession;
        return enqueueCommand(command);
    }

    bool requestSendMcp(const std::string& payload) {
        if (payload.empty() ||
            payload.size() > runtime_config_.maximum_mcp_payload_bytes ||
            !running_.load() || stop_requested_.load()) {
            ++commands_rejected_;
            return false;
        }
        Command command;
        command.type = CommandType::SendMcp;
        McpCommandSlot* slot = mcp_command_pool_.acquire(command.payload_slot);
        if (slot == nullptr) {
            ++command_pool_exhausted_;
            ++commands_rejected_;
            return false;
        }
        noteCommandPayloadAcquired();
        slot->payload.assign(payload.data(), payload.size());
        return enqueueCommand(command);
    }

    bool notifyWakeWordDetected(const char* wake_word, size_t size,
                                bool from_isr) override {
        if (wake_word == nullptr || size == 0 ||
            size > RealtimeControlSink::kMaximumWakeWordBytes ||
            wake_event_queue_ == nullptr || !running_.load() ||
            stop_requested_.load()) {
            ++wake_events_rejected_;
            ++urgent_controls_rejected_;
            return false;
        }
        WakeEvent event;
        event.size = static_cast<uint8_t>(size);
        std::copy_n(wake_word, size, event.text);
        event.text[size] = '\0';
        BaseType_t queued = pdFALSE;
        bool coalesced = false;
        if (from_isr) {
            BaseType_t higher_priority_task_woken = pdFALSE;
            queued = xQueueSendFromISR(wake_event_queue_, &event,
                                       &higher_priority_task_woken);
            if (queued != pdTRUE) {
                queued = xQueueOverwriteFromISR(wake_event_queue_, &event,
                                                &higher_priority_task_woken);
                coalesced = queued == pdTRUE;
            }
        } else {
            queued = xQueueSend(wake_event_queue_, &event, 0);
            if (queued != pdTRUE) {
                queued = xQueueOverwrite(wake_event_queue_, &event);
                coalesced = queued == pdTRUE;
            }
        }
        if (queued != pdTRUE) {
            ++wake_events_rejected_;
            ++urgent_controls_rejected_;
            return false;
        }
        if (coalesced) {
            // Retain the newest fixed payload while one wake intent is pending.
            ++wake_events_coalesced_;
            ++urgent_controls_coalesced_;
        } else {
            ++wake_events_queued_;
            ++urgent_controls_queued_;
        }
        notifyServiceTask(from_isr);
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
        output.urgent_controls_queued = urgent_controls_queued_.load();
        output.urgent_controls_executed = urgent_controls_executed_.load();
        output.urgent_controls_coalesced = urgent_controls_coalesced_.load();
        output.urgent_controls_rejected = urgent_controls_rejected_.load();
        output.command_pool_exhausted = command_pool_exhausted_.load();
        output.callback_pool_exhausted = callback_pool_exhausted_.load();
        output.state_callbacks_dropped = state_callbacks_dropped_.load();
        output.wake_callbacks_dropped = wake_callbacks_dropped_.load();
        output.event_callbacks_dropped = event_callbacks_dropped_.load();
        output.audio_callbacks_dropped = audio_callbacks_dropped_.load();
        output.capture_callbacks_dropped = capture_callbacks_dropped_.load();
        output.error_callbacks_dropped = error_callbacks_dropped_.load();
        output.audio_meta_callbacks_dropped =
            audio_meta_callbacks_dropped_.load();
        output.state_callbacks_coalesced = state_callbacks_coalesced_.load();
        output.capture_callbacks_coalesced =
            capture_callbacks_coalesced_.load();
        output.command_payload_pool_high_watermark =
            command_payload_pool_high_watermark_.load();
        output.callback_payload_pool_high_watermark =
            callback_payload_pool_high_watermark_.load();
        output.command_queue_high_watermark = command_queue_high_watermark_.load();
        output.callback_queue_high_watermark = callback_queue_high_watermark_.load();
        output.audio_callback_queue_high_watermark =
            audio_callback_queue_high_watermark_.load();
        output.service_cycle_overruns = service_cycle_overruns_.load();
        output.service_cycle_maximum_us = service_cycle_maximum_us_.load();
        return output;
    }

private:
    enum class CommandType : uint8_t {
        StartListening,
        ToggleChat,
        CloseSession,
        SendMcp,
    };

    struct Command {
        CommandType type = CommandType::ToggleChat;
        ListeningMode listening_mode = ListeningMode::ManualStop;
        uint8_t payload_slot = 0xFF;
    };

    struct McpCommandSlot {
        std::string payload;
        void clear() { payload.clear(); }
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
        AudioMeta,
        Capture,
        Error,
    };

    struct CallbackMessage {
        CallbackType type = CallbackType::StateChanged;
        State old_state = State::Unknown;
        State new_state = State::Unknown;
        bool capture_enabled = false;
        AudioFormat capture_format;
        AudioFrameMeta audio_meta;
        uint8_t payload_slot = 0xFF;
    };

    struct WakeCallbackSlot {
        std::string text;
        void clear() { text.clear(); }
    };

    struct EventCallbackSlot {
        Event event;
        void clear() {
            event.type = EventType::UnknownMessage;
            event.text.clear();
            event.status.clear();
            event.emotion.clear();
            event.json.clear();
            event.emotion_type = Emotion::Unknown;
        }
    };

    struct AudioCallbackSlot {
        AudioFrame audio;
        void clear() {
            audio.format = {};
            audio.timestamp = 0;
            audio.opus.clear();
        }
    };

    struct ErrorCallbackSlot {
        ErrorCode code = ErrorCode::None;
        std::string message;
        void clear() {
            code = ErrorCode::None;
            message.clear();
        }
    };

    static_assert(std::is_trivially_copyable<Command>::value,
                  "FreeRTOS command queues require byte-copyable messages");
    static_assert(std::is_trivially_copyable<CallbackMessage>::value,
                  "FreeRTOS callback queues require byte-copyable messages");

    Client& client_;
    ClientConfig client_config_;
    ClientRuntimeConfig runtime_config_;
    Callbacks user_callbacks_;
    QueueHandle_t command_queue_ = nullptr;
    QueueHandle_t callback_queue_ = nullptr;
    QueueHandle_t state_callback_queue_ = nullptr;
    QueueHandle_t capture_callback_queue_ = nullptr;
    QueueHandle_t audio_callback_queue_ = nullptr;
    QueueHandle_t wake_event_queue_ = nullptr;
    SemaphoreHandle_t startup_signal_ = nullptr;
    SemaphoreHandle_t stopped_signal_ = nullptr;
    FixedSlotPool<McpCommandSlot> mcp_command_pool_;
    FixedSlotPool<WakeCallbackSlot> wake_callback_pool_;
    FixedSlotPool<EventCallbackSlot> event_callback_pool_;
    FixedSlotPool<AudioCallbackSlot> audio_callback_pool_;
    FixedSlotPool<ErrorCallbackSlot> error_callback_pool_;
    std::string wake_word_scratch_;
    std::atomic<bool> task_active_{false};
    std::atomic<TaskHandle_t> service_task_handle_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> session_ready_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> startup_succeeded_{false};
    std::atomic<uint32_t> callback_dispatch_depth_{0};
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
    std::atomic<uint32_t> pending_urgent_controls_{0};
    std::atomic<uint8_t> pending_abort_reason_{
        static_cast<uint8_t>(AbortReason::None)};
    std::atomic<bool> pending_playback_mute_{false};
    std::atomic<uint32_t> urgent_controls_queued_{0};
    std::atomic<uint32_t> urgent_controls_executed_{0};
    std::atomic<uint32_t> urgent_controls_coalesced_{0};
    std::atomic<uint32_t> urgent_controls_rejected_{0};
    std::atomic<uint32_t> command_pool_exhausted_{0};
    std::atomic<uint32_t> callback_pool_exhausted_{0};
    std::atomic<uint32_t> state_callbacks_dropped_{0};
    std::atomic<uint32_t> wake_callbacks_dropped_{0};
    std::atomic<uint32_t> event_callbacks_dropped_{0};
    std::atomic<uint32_t> audio_callbacks_dropped_{0};
    std::atomic<uint32_t> capture_callbacks_dropped_{0};
    std::atomic<uint32_t> error_callbacks_dropped_{0};
    std::atomic<uint32_t> audio_meta_callbacks_dropped_{0};
    std::atomic<uint32_t> state_callbacks_coalesced_{0};
    std::atomic<uint32_t> capture_callbacks_coalesced_{0};
    std::atomic<uint8_t> command_payloads_in_use_{0};
    std::atomic<uint8_t> callback_payloads_in_use_{0};
    std::atomic<uint8_t> command_payload_pool_high_watermark_{0};
    std::atomic<uint8_t> callback_payload_pool_high_watermark_{0};
    std::atomic<uint8_t> command_queue_high_watermark_{0};
    std::atomic<uint8_t> callback_queue_high_watermark_{0};
    std::atomic<uint8_t> audio_callback_queue_high_watermark_{0};
    std::atomic<uint32_t> service_cycle_overruns_{0};
    std::atomic<uint32_t> service_cycle_maximum_us_{0};

    // Owned exclusively by the Runtime service task. uint32_t subtraction is
    // intentionally wrap-safe across the Arduino millisecond counter rollover.
    uint32_t speaking_started_ms_ = 0;
    uint32_t first_downlink_audio_ms_ = 0;
    uint32_t last_downlink_audio_ms_ = 0;
    bool playback_watchdog_armed_ = false;
    bool received_downlink_audio_ = false;
    uint32_t playback_retry_started_ms_ = 0;
    bool playback_retry_pending_ = false;

    static constexpr uint32_t kUrgentAbortSpeaking = 1U << 0;
    static constexpr uint32_t kUrgentStopListening = 1U << 1;
    static constexpr uint32_t kUrgentPlaybackMute = 1U << 2;

    bool publishUrgentControl(uint32_t control, bool from_isr) {
        if (!running_.load() || stop_requested_.load()) {
            ++urgent_controls_rejected_;
            return false;
        }
        const uint32_t previous =
            pending_urgent_controls_.fetch_or(control, std::memory_order_release);
        if ((previous & control) != 0) {
            ++urgent_controls_coalesced_;
        } else {
            ++urgent_controls_queued_;
        }
        notifyServiceTask(from_isr);
        return true;
    }

    static void serviceTaskEntry(void* argument) {
        static_cast<Impl*>(argument)->serviceTask();
    }

    static void transportEventReady(void* context) {
        static_cast<Impl*>(context)->notifyServiceTask(false);
    }

    static void audioEventReady(void* context) {
        static_cast<Impl*>(context)->notifyServiceTask(false);
    }

    void notifyServiceTask(bool from_isr) {
        TaskHandle_t task =
            service_task_handle_.load(std::memory_order_acquire);
        if (task == nullptr) {
            return;
        }
        if (from_isr) {
            BaseType_t higher_priority_task_woken = pdFALSE;
            vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
            if (higher_priority_task_woken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
            return;
        }
        xTaskNotifyGive(task);
    }

    bool validConfig(const ClientRuntimeConfig& config) const {
        return config.task_stack_size >= 4096 && config.task_priority > 0 &&
               config.task_priority < configMAX_PRIORITIES &&
               config.task_core >= -1 && config.task_core < portNUM_PROCESSORS &&
               config.poll_interval_ms > 0 && config.poll_interval_ms <= 1000 &&
               config.idle_poll_interval_ms > 0 &&
               config.idle_poll_interval_ms <= 1000 &&
               config.command_queue_depth > 0 && config.callback_queue_depth > 0 &&
               config.audio_callback_queue_depth > 0 &&
               config.maximum_service_cycle_us > 0 &&
               config.maximum_service_cycle_us <= 1000000 &&
               config.maximum_commands_per_cycle > 0 &&
               config.mcp_command_pool_depth > 0 &&
               config.mcp_command_pool_depth <= 32 &&
               config.event_callback_pool_depth > 0 &&
               config.event_callback_pool_depth <= 32 &&
               config.audio_callback_pool_depth > 0 &&
               config.audio_callback_pool_depth <= 32 &&
               config.error_callback_pool_depth > 0 &&
               config.error_callback_pool_depth <= 32 &&
               config.wake_callback_pool_depth > 0 &&
               config.wake_callback_pool_depth <= 32 &&
               config.maximum_mcp_payload_bytes > 0 &&
               config.maximum_mcp_payload_bytes <= 16384 &&
               config.maximum_event_text_bytes > 0 &&
               config.maximum_event_text_bytes <= 16384 &&
               config.maximum_event_status_bytes > 0 &&
               config.maximum_event_status_bytes <= 1024 &&
               config.maximum_event_emotion_bytes > 0 &&
               config.maximum_event_emotion_bytes <= 256 &&
               config.maximum_event_json_bytes > 0 &&
               config.maximum_event_json_bytes <= 16384 &&
               config.maximum_audio_callback_bytes > 0 &&
               config.maximum_audio_callback_bytes <= 8192 &&
               config.maximum_error_message_bytes > 0 &&
               config.maximum_error_message_bytes <= 2048 &&
               (!config.playback_watchdog.enabled ||
                (config.playback_watchdog.first_audio_timeout_ms > 0 &&
                 config.playback_watchdog.first_audio_timeout_ms <= 3600000 &&
                 config.playback_watchdog.inter_packet_timeout_ms > 0 &&
                 config.playback_watchdog.inter_packet_timeout_ms <= 3600000)) &&
               (config.lifecycle.on_state_changed == nullptr ||
                (config.lifecycle.maximum_execution_us > 0 &&
                 config.lifecycle.maximum_execution_us <= 1000000));
    }

    bool initializePayloadPools() {
        if (!mcp_command_pool_.initialize(
                runtime_config_.mcp_command_pool_depth) ||
            (user_callbacks_.on_wake_word &&
             !wake_callback_pool_.initialize(
                 runtime_config_.wake_callback_pool_depth)) ||
            (user_callbacks_.on_event &&
             !event_callback_pool_.initialize(
                 runtime_config_.event_callback_pool_depth)) ||
            (user_callbacks_.on_audio &&
             !audio_callback_pool_.initialize(
                 runtime_config_.audio_callback_pool_depth)) ||
            (user_callbacks_.on_error &&
             !error_callback_pool_.initialize(
                 runtime_config_.error_callback_pool_depth))) {
            return false;
        }
        for (uint8_t index = 0;
             index < runtime_config_.mcp_command_pool_depth; ++index) {
            mcp_command_pool_.at(index).payload.reserve(
                runtime_config_.maximum_mcp_payload_bytes);
        }
        wake_word_scratch_.reserve(RealtimeControlSink::kMaximumWakeWordBytes);
        if (user_callbacks_.on_wake_word) {
            for (uint8_t index = 0;
                 index < runtime_config_.wake_callback_pool_depth; ++index) {
                wake_callback_pool_.at(index).text.reserve(
                    RealtimeControlSink::kMaximumWakeWordBytes);
            }
        }
        if (user_callbacks_.on_event) {
            for (uint8_t index = 0;
                 index < runtime_config_.event_callback_pool_depth; ++index) {
                Event& event = event_callback_pool_.at(index).event;
                event.text.reserve(runtime_config_.maximum_event_text_bytes);
                event.status.reserve(runtime_config_.maximum_event_status_bytes);
                event.emotion.reserve(
                    runtime_config_.maximum_event_emotion_bytes);
                event.json.reserve(runtime_config_.maximum_event_json_bytes);
            }
        }
        if (user_callbacks_.on_audio) {
            for (uint8_t index = 0;
                 index < runtime_config_.audio_callback_pool_depth; ++index) {
                audio_callback_pool_.at(index).audio.opus.reserve(
                    runtime_config_.maximum_audio_callback_bytes);
            }
        }
        if (user_callbacks_.on_error) {
            for (uint8_t index = 0;
                 index < runtime_config_.error_callback_pool_depth; ++index) {
                error_callback_pool_.at(index).message.reserve(
                    runtime_config_.maximum_error_message_bytes);
            }
        }
        return true;
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
                    CallbackMessage message;
                    message.type = CallbackType::StateChanged;
                    message.old_state = old_state;
                    message.new_state = new_state;
                    enqueueCallback(message);
                }
            };
        }
        if (user_callbacks_.on_event) {
            callbacks.on_event = [this](const Event& event) {
                if (event.text.size() > runtime_config_.maximum_event_text_bytes ||
                    event.status.size() >
                        runtime_config_.maximum_event_status_bytes ||
                    event.emotion.size() >
                        runtime_config_.maximum_event_emotion_bytes ||
                    event.json.size() > runtime_config_.maximum_event_json_bytes) {
                    recordCallbackDrop(CallbackType::Event);
                    return;
                }
                CallbackMessage message;
                message.type = CallbackType::Event;
                EventCallbackSlot* slot =
                    event_callback_pool_.acquire(message.payload_slot);
                if (slot == nullptr) {
                    ++callback_pool_exhausted_;
                    recordCallbackDrop(CallbackType::Event);
                    return;
                }
                noteCallbackPayloadAcquired();
                slot->event.type = event.type;
                slot->event.text.assign(event.text.data(), event.text.size());
                slot->event.status.assign(event.status.data(), event.status.size());
                slot->event.emotion.assign(event.emotion.data(), event.emotion.size());
                slot->event.json.assign(event.json.data(), event.json.size());
                slot->event.emotion_type = event.emotion_type;
                enqueueCallback(message);
            };
        }
        if (user_callbacks_.on_audio_meta ||
            runtime_config_.playback_watchdog.enabled) {
            callbacks.on_audio_meta = [this](const AudioFrameMeta& meta) {
                noteDownlinkAudio();
                if (!user_callbacks_.on_audio_meta) {
                    return;
                }
                CallbackMessage message;
                message.type = CallbackType::AudioMeta;
                message.audio_meta = meta;
                enqueueCallback(message);
            };
        }
        if (user_callbacks_.on_audio) {
            callbacks.on_audio = [this](const AudioFrame& audio) {
                // Audio callbacks are observers; attached EncodedAudioPort
                // playback is delivered directly by Client. Avoid allocating
                // and copying Opus when a stalled user loop has filled its
                // best-effort callback queue.
                if (audio_callback_queue_ == nullptr ||
                    uxQueueSpacesAvailable(audio_callback_queue_) == 0) {
                    recordCallbackDrop(CallbackType::Audio);
                    return;
                }
                if (audio.opus.size() >
                    runtime_config_.maximum_audio_callback_bytes) {
                    recordCallbackDrop(CallbackType::Audio);
                    return;
                }
                CallbackMessage message;
                message.type = CallbackType::Audio;
                AudioCallbackSlot* slot =
                    audio_callback_pool_.acquire(message.payload_slot);
                if (slot == nullptr) {
                    ++callback_pool_exhausted_;
                    recordCallbackDrop(CallbackType::Audio);
                    return;
                }
                noteCallbackPayloadAcquired();
                slot->audio.format = audio.format;
                slot->audio.timestamp = audio.timestamp;
                slot->audio.opus.assign(audio.opus.begin(), audio.opus.end());
                enqueueCallback(message);
            };
        }
        if (user_callbacks_.on_capture) {
            callbacks.on_capture = [this](bool enabled, const AudioFormat& format) {
                CallbackMessage message;
                message.type = CallbackType::Capture;
                message.capture_enabled = enabled;
                message.capture_format = format;
                enqueueCallback(message);
            };
        }
        if (user_callbacks_.on_error) {
            callbacks.on_error = [this](ErrorCode code, const std::string& error) {
                if (error.size() > runtime_config_.maximum_error_message_bytes) {
                    recordCallbackDrop(CallbackType::Error);
                    return;
                }
                CallbackMessage message;
                message.type = CallbackType::Error;
                ErrorCallbackSlot* slot =
                    error_callback_pool_.acquire(message.payload_slot);
                if (slot == nullptr) {
                    ++callback_pool_exhausted_;
                    recordCallbackDrop(CallbackType::Error);
                    return;
                }
                noteCallbackPayloadAcquired();
                slot->code = code;
                slot->message.assign(error.data(), error.size());
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
            playback_retry_pending_ = false;
            refreshPlaybackSnapshot();
            return;
        }
        playback_watchdog_armed_ = false;
        received_downlink_audio_ = false;
        speaking_started_ms_ = 0;
        first_downlink_audio_ms_ = 0;
        last_downlink_audio_ms_ = 0;
        playback_retry_pending_ = false;
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
        playback_retry_pending_ = false;
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
        // Sampling is deferred until the deadline so notification-driven
        // service does not contend with audio workers on every event.
        refreshPlaybackSnapshot();
        if (!playback_idle_.load()) {
            playback_retry_started_ms_ = now_ms;
            playback_retry_pending_ = true;
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

    uint32_t nextPlaybackDeadlineMs() const {
        if (!runtime_config_.playback_watchdog.enabled ||
            !playback_watchdog_armed_ || client_.state() != State::Speaking) {
            return std::numeric_limits<uint32_t>::max();
        }
        const uint32_t now_ms = runtimeNowMs();
        const uint32_t reference_ms = received_downlink_audio_
                                          ? last_downlink_audio_ms_
                                          : speaking_started_ms_;
        const uint32_t timeout_ms =
            received_downlink_audio_
                ? runtime_config_.playback_watchdog.inter_packet_timeout_ms
                : runtime_config_.playback_watchdog.first_audio_timeout_ms;
        const uint32_t elapsed_ms = now_ms - reference_ms;
        if (elapsed_ms < timeout_ms) {
            return timeout_ms - elapsed_ms;
        }
        if (!playback_retry_pending_) {
            return 0;
        }
        const uint32_t retry_elapsed_ms = now_ms - playback_retry_started_ms_;
        return retry_elapsed_ms >= runtime_config_.idle_poll_interval_ms
                   ? 0
                   : runtime_config_.idle_poll_interval_ms - retry_elapsed_ms;
    }

    uint32_t nextServiceDelayMs() const {
        // Notifications are coalesced (ulTaskNotifyTake clears their count).
        // A bounded command batch can leave work after the last notification
        // has been consumed; never sleep until a network/audio deadline then.
        if (uxQueueMessagesWaiting(command_queue_) != 0 ||
            uxQueueMessagesWaiting(wake_event_queue_) != 0 ||
            pending_urgent_controls_.load(std::memory_order_acquire) != 0) {
            return 0;
        }
        uint32_t delay_ms = client_.nextProtocolDeadlineMs();
        delay_ms = std::min(delay_ms, nextPlaybackDeadlineMs());
        if (client_.pollingRequired()) {
            delay_ms = std::min<uint32_t>(delay_ms,
                                          runtime_config_.poll_interval_ms);
        }
        return delay_ms;
    }

    void serviceTask() {
        service_task_handle_.store(xTaskGetCurrentTaskHandle(),
                                   std::memory_order_release);
        const bool sink_attached = client_.attachRealtimeControlSink(this);
        TransportEventNotifier notifier;
        notifier.notify = transportEventReady;
        notifier.context = this;
        const bool transport_notifier_attached =
            sink_attached && client_.setTransportEventNotifier(notifier);
        AudioEventNotifier audio_notifier;
        audio_notifier.notify = audioEventReady;
        audio_notifier.context = this;
        const bool audio_notifier_attached =
            transport_notifier_attached &&
            client_.setAudioEventNotifier(audio_notifier);
        const bool begun = audio_notifier_attached &&
                           client_.begin(std::move(client_config_),
                                         makeDeferredCallbacks());
        // client_config_ only bridges the caller and service tasks. Client now
        // owns the reconnect fields, so keep no duplicate strings in Runtime.
        ClientConfig empty_client_config;
        std::swap(client_config_, empty_client_config);
        startup_succeeded_.store(begun);
        running_.store(begun);
        snapshotClient();
        xSemaphoreGive(startup_signal_);

        if (begun) {
            while (!stop_requested_.load()) {
                const int64_t cycle_started_us = esp_timer_get_time();
                processUrgentControls();
                servicePlaybackWatchdog();
                processCommands();
                if (stop_requested_.load()) {
                    break;
                }
                client_.loop();
                snapshotClient();
                const uint32_t cycle_elapsed_us = static_cast<uint32_t>(
                    std::min<int64_t>(
                        std::max<int64_t>(0, esp_timer_get_time() -
                                                cycle_started_us),
                        std::numeric_limits<uint32_t>::max()));
                updateMaximum(service_cycle_maximum_us_, cycle_elapsed_us);
                if (cycle_elapsed_us >
                    runtime_config_.maximum_service_cycle_us) {
                    ++service_cycle_overruns_;
                }
                ++service_cycles_;
                ulTaskNotifyTake(pdTRUE, timeoutTicks(nextServiceDelayMs()));
            }
            discardCommands();
            client_.end();
        }

        if (audio_notifier_attached) {
            client_.setAudioEventNotifier({});
        }

        if (transport_notifier_attached) {
            client_.setTransportEventNotifier({});
        }

        if (sink_attached) {
            client_.attachRealtimeControlSink(nullptr);
        }

        snapshotClient();
        running_.store(false);
        service_task_handle_.store(nullptr, std::memory_order_release);
        // Keep task_active_ true until end() consumes the completion signal.
        // Publishing false here lets end() free stopped_signal_ (or this Impl)
        // before the worker has finished accessing it.
        xSemaphoreGive(stopped_signal_);
        vTaskDelete(nullptr);
    }

    void processCommands() {
        for (uint8_t index = 0; index < runtime_config_.maximum_commands_per_cycle;
             ++index) {
            Command command;
            if (xQueueReceive(command_queue_, &command, 0) != pdTRUE) {
                return;
            }
            executeCommand(command);
            snapshotClient();
            if (stop_requested_.load()) {
                return;
            }
        }
    }

    void processUrgentControls() {
        const uint32_t controls =
            pending_urgent_controls_.exchange(0, std::memory_order_acquire);
        if ((controls & kUrgentAbortSpeaking) != 0) {
            const AbortReason reason = static_cast<AbortReason>(
                pending_abort_reason_.load(std::memory_order_relaxed));
            const bool accepted = client_.abortSpeaking(reason);
            ++urgent_controls_executed_;
            if (!accepted) {
                ++urgent_controls_rejected_;
            }
            snapshotClient();
        }
        if ((controls & kUrgentStopListening) != 0) {
            const bool accepted = client_.stopListening();
            ++urgent_controls_executed_;
            if (!accepted) {
                ++urgent_controls_rejected_;
            }
            snapshotClient();
        }
        if ((controls & kUrgentPlaybackMute) != 0) {
            const bool accepted =
                client_.setPlaybackMuted(pending_playback_mute_.load());
            ++urgent_controls_executed_;
            if (!accepted) {
                ++urgent_controls_rejected_;
            }
        }
        // Wake is intentionally last: if abort/stop and wake arrive together,
        // the final user intent is to begin the newest wake turn.
        processWakeEvents();
    }

    void processWakeEvents() {
        if (wake_event_queue_ == nullptr) {
            return;
        }
        WakeEvent event;
        if (xQueueReceive(wake_event_queue_, &event, 0) != pdTRUE) {
            return;
        }
        ++urgent_controls_executed_;
        const State current = client_.state();
        if (!client_.ready() ||
            (current != State::Idle && current != State::Listening &&
             current != State::Speaking && current != State::Connecting)) {
            // Other transient states cannot retain a voice-session wake intent.
            ++wake_events_rejected_;
            ++urgent_controls_rejected_;
            return;
        }
        wake_word_scratch_.assign(event.text, event.size);
        if (!client_.wakeWordDetected(wake_word_scratch_)) {
            ++wake_events_rejected_;
            ++urgent_controls_rejected_;
            return;
        }
        ++wake_events_executed_;
        enqueueWakeObserver(wake_word_scratch_.data(), wake_word_scratch_.size());
    }

    void noteCommandPayloadAcquired() {
        const uint8_t in_use = command_payloads_in_use_.fetch_add(1) + 1;
        updateHighWatermark(command_payload_pool_high_watermark_, in_use);
    }

    void releaseCommandPayload(const Command& command) {
        if (command.type == CommandType::SendMcp && command.payload_slot != 0xFF) {
            mcp_command_pool_.release(command.payload_slot);
            command_payloads_in_use_.fetch_sub(1);
        }
    }

    bool enqueueCommand(const Command& command) {
        if (command_queue_ == nullptr || !running_.load() ||
            stop_requested_.load()) {
            releaseCommandPayload(command);
            ++commands_rejected_;
            return false;
        }
        if (xQueueSend(command_queue_, &command, 0) != pdTRUE) {
            releaseCommandPayload(command);
            ++commands_rejected_;
            return false;
        }
        ++commands_queued_;
        updateHighWatermark(command_queue_high_watermark_,
                            uxQueueMessagesWaiting(command_queue_));
        notifyServiceTask(false);
        return true;
    }

    void noteCallbackPayloadAcquired() {
        const uint8_t in_use = callback_payloads_in_use_.fetch_add(1) + 1;
        updateHighWatermark(callback_payload_pool_high_watermark_, in_use);
    }

    void recordCallbackDrop(CallbackType type) {
        ++callbacks_dropped_;
        switch (type) {
            case CallbackType::StateChanged:
                ++state_callbacks_dropped_;
                break;
            case CallbackType::WakeWord:
                ++wake_callbacks_dropped_;
                break;
            case CallbackType::Event:
                ++event_callbacks_dropped_;
                break;
            case CallbackType::Audio:
                ++audio_callbacks_dropped_;
                break;
            case CallbackType::AudioMeta:
                ++audio_meta_callbacks_dropped_;
                break;
            case CallbackType::Capture:
                ++capture_callbacks_dropped_;
                break;
            case CallbackType::Error:
                ++error_callbacks_dropped_;
                break;
        }
    }

    void releaseCallbackPayload(const CallbackMessage& message) {
        if (message.payload_slot == 0xFF) {
            return;
        }
        switch (message.type) {
            case CallbackType::WakeWord:
                wake_callback_pool_.release(message.payload_slot);
                break;
            case CallbackType::Event:
                event_callback_pool_.release(message.payload_slot);
                break;
            case CallbackType::Audio:
                audio_callback_pool_.release(message.payload_slot);
                break;
            case CallbackType::Error:
                error_callback_pool_.release(message.payload_slot);
                break;
            case CallbackType::StateChanged:
            case CallbackType::AudioMeta:
            case CallbackType::Capture:
                return;
        }
        callback_payloads_in_use_.fetch_sub(1);
    }

    void enqueueCallback(const CallbackMessage& message) {
        if (message.type == CallbackType::StateChanged) {
            if (state_callback_queue_ == nullptr) {
                recordCallbackDrop(message.type);
                return;
            }
            if (uxQueueMessagesWaiting(state_callback_queue_) != 0) {
                ++state_callbacks_coalesced_;
            }
            if (xQueueOverwrite(state_callback_queue_, &message) != pdTRUE) {
                recordCallbackDrop(message.type);
                return;
            }
            ++callbacks_queued_;
            return;
        }
        if (message.type == CallbackType::Capture) {
            if (capture_callback_queue_ == nullptr) {
                recordCallbackDrop(message.type);
                return;
            }
            if (uxQueueMessagesWaiting(capture_callback_queue_) != 0) {
                ++capture_callbacks_coalesced_;
            }
            if (xQueueOverwrite(capture_callback_queue_, &message) != pdTRUE) {
                recordCallbackDrop(message.type);
                return;
            }
            ++callbacks_queued_;
            return;
        }
        if (message.type == CallbackType::Audio ||
            message.type == CallbackType::AudioMeta) {
            if (audio_callback_queue_ == nullptr ||
                xQueueSend(audio_callback_queue_, &message, 0) != pdTRUE) {
                releaseCallbackPayload(message);
                recordCallbackDrop(message.type);
                return;
            }
            ++callbacks_queued_;
            updateHighWatermark(audio_callback_queue_high_watermark_,
                                uxQueueMessagesWaiting(audio_callback_queue_));
            return;
        }
        if (callback_queue_ == nullptr) {
            releaseCallbackPayload(message);
            recordCallbackDrop(message.type);
            return;
        }
        if (xQueueSend(callback_queue_, &message, 0) != pdTRUE) {
            // Accepted reliable callbacks are never displaced by UI state or
            // best-effort audio observers. A full bounded queue rejects only
            // the new reliable item and records its exact type.
            releaseCallbackPayload(message);
            recordCallbackDrop(message.type);
            return;
        }
        ++callbacks_queued_;
        updateHighWatermark(callback_queue_high_watermark_,
                            uxQueueMessagesWaiting(callback_queue_));
    }

    bool receiveNextCallback(CallbackMessage& message) {
        return (callback_queue_ != nullptr &&
                xQueueReceive(callback_queue_, &message, 0) == pdTRUE) ||
               (state_callback_queue_ != nullptr &&
                xQueueReceive(state_callback_queue_, &message, 0) == pdTRUE) ||
               (capture_callback_queue_ != nullptr &&
                xQueueReceive(capture_callback_queue_, &message, 0) == pdTRUE) ||
               (audio_callback_queue_ != nullptr &&
                xQueueReceive(audio_callback_queue_, &message, 0) == pdTRUE);
    }

    void enqueueWakeObserver(const char* wake_word, size_t size) {
        if (!user_callbacks_.on_wake_word) {
            return;
        }
        CallbackMessage message;
        message.type = CallbackType::WakeWord;
        WakeCallbackSlot* slot =
            wake_callback_pool_.acquire(message.payload_slot);
        if (slot == nullptr) {
            ++callback_pool_exhausted_;
            recordCallbackDrop(CallbackType::WakeWord);
            return;
        }
        noteCallbackPayloadAcquired();
        slot->text.assign(wake_word, size);
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
                    user_callbacks_.on_wake_word(
                        wake_callback_pool_.at(message.payload_slot).text);
                }
                break;
            case CallbackType::Event:
                if (user_callbacks_.on_event) {
                    user_callbacks_.on_event(
                        event_callback_pool_.at(message.payload_slot).event);
                }
                break;
            case CallbackType::Audio:
                if (user_callbacks_.on_audio) {
                    user_callbacks_.on_audio(
                        audio_callback_pool_.at(message.payload_slot).audio);
                }
                break;
            case CallbackType::AudioMeta:
                if (user_callbacks_.on_audio_meta) {
                    user_callbacks_.on_audio_meta(message.audio_meta);
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
                    const ErrorCallbackSlot& slot =
                        error_callback_pool_.at(message.payload_slot);
                    user_callbacks_.on_error(slot.code, slot.message);
                }
                break;
        }
    }

    void snapshotClient() {
        ready_.store(client_.ready());
        session_ready_.store(client_.sessionReady());
        state_.store(client_.state());
    }

    void executeCommand(const Command& command) {
        bool accepted = false;
        switch (command.type) {
            case CommandType::StartListening:
                accepted = client_.startListening(command.listening_mode);
                break;
            case CommandType::ToggleChat:
                accepted = client_.toggleChat();
                break;
            case CommandType::CloseSession:
                accepted = client_.closeSession();
                break;
            case CommandType::SendMcp:
                accepted = client_.sendMcp(
                    mcp_command_pool_.at(command.payload_slot).payload);
                break;
        }
        ++commands_executed_;
        if (!accepted) {
            ++commands_rejected_;
        }
        releaseCommandPayload(command);
    }

    void discardCommands() {
        if (command_queue_ == nullptr) {
            return;
        }
        Command command;
        while (xQueueReceive(command_queue_, &command, 0) == pdTRUE) {
            releaseCommandPayload(command);
        }
    }

    void discardCallbacks() {
        discardCallbackQueue(callback_queue_);
        discardCallbackQueue(state_callback_queue_);
        discardCallbackQueue(capture_callback_queue_);
        discardCallbackQueue(audio_callback_queue_);
    }

    void discardCallbackQueue(QueueHandle_t queue) {
        if (queue == nullptr) {
            return;
        }
        CallbackMessage message;
        while (xQueueReceive(queue, &message, 0) == pdTRUE) {
            releaseCallbackPayload(message);
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
        if (state_callback_queue_ != nullptr) {
            vQueueDelete(state_callback_queue_);
            state_callback_queue_ = nullptr;
        }
        if (capture_callback_queue_ != nullptr) {
            vQueueDelete(capture_callback_queue_);
            capture_callback_queue_ = nullptr;
        }
        if (audio_callback_queue_ != nullptr) {
            vQueueDelete(audio_callback_queue_);
            audio_callback_queue_ = nullptr;
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
        mcp_command_pool_.reset();
        wake_callback_pool_.reset();
        event_callback_pool_.reset();
        audio_callback_pool_.reset();
        error_callback_pool_.reset();
        wake_word_scratch_.clear();
        ready_.store(false);
        session_ready_.store(false);
        pending_urgent_controls_.store(0);
        service_task_handle_.store(nullptr, std::memory_order_release);
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
        pending_urgent_controls_.store(0);
        pending_abort_reason_.store(static_cast<uint8_t>(AbortReason::None));
        pending_playback_mute_.store(false);
        urgent_controls_queued_.store(0);
        urgent_controls_executed_.store(0);
        urgent_controls_coalesced_.store(0);
        urgent_controls_rejected_.store(0);
        command_pool_exhausted_.store(0);
        callback_pool_exhausted_.store(0);
        state_callbacks_dropped_.store(0);
        wake_callbacks_dropped_.store(0);
        event_callbacks_dropped_.store(0);
        audio_callbacks_dropped_.store(0);
        capture_callbacks_dropped_.store(0);
        error_callbacks_dropped_.store(0);
        audio_meta_callbacks_dropped_.store(0);
        state_callbacks_coalesced_.store(0);
        capture_callbacks_coalesced_.store(0);
        command_payloads_in_use_.store(0);
        callback_payloads_in_use_.store(0);
        command_payload_pool_high_watermark_.store(0);
        callback_payload_pool_high_watermark_.store(0);
        speaking_started_ms_ = 0;
        first_downlink_audio_ms_ = 0;
        last_downlink_audio_ms_ = 0;
        playback_watchdog_armed_ = false;
        received_downlink_audio_ = false;
        playback_retry_started_ms_ = 0;
        playback_retry_pending_ = false;
        command_queue_high_watermark_.store(0);
        callback_queue_high_watermark_.store(0);
        audio_callback_queue_high_watermark_.store(0);
        service_cycle_overruns_.store(0);
        service_cycle_maximum_us_.store(0);
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

bool ClientRuntime::requestWakeWordDetected(const char* wake_word, size_t size) {
    return impl_->requestWakeWordDetected(wake_word, size);
}

bool ClientRuntime::requestPlaybackMute(bool muted) {
    return impl_->requestPlaybackMute(muted);
}

bool ClientRuntime::requestStopListeningFromISR() {
    return impl_->requestStopListeningFromISR();
}

bool ClientRuntime::requestAbortSpeakingFromISR(AbortReason reason) {
    return impl_->requestAbortSpeakingFromISR(reason);
}

bool ClientRuntime::requestWakeWordDetectedFromISR(const char* wake_word,
                                                   size_t size) {
    return impl_->requestWakeWordDetectedFromISR(wake_word, size);
}

bool ClientRuntime::requestPlaybackMuteFromISR(bool muted) {
    return impl_->requestPlaybackMuteFromISR(muted);
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
