#include "AsyncTransport.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace xiaozhi {
namespace {

constexpr uint8_t kInvalidSlot = std::numeric_limits<uint8_t>::max();

TickType_t timeoutTicks(uint32_t timeout_ms) {
    if (timeout_ms == std::numeric_limits<uint32_t>::max()) {
        return portMAX_DELAY;
    }
    if (timeout_ms == 0) {
        return 0;
    }
    return std::max<TickType_t>(1, pdMS_TO_TICKS(timeout_ms));
}

uint32_t elapsedUs(int64_t started_us) {
    const int64_t elapsed = std::max<int64_t>(0, esp_timer_get_time() - started_us);
    return static_cast<uint32_t>(std::min<int64_t>(
        elapsed, std::numeric_limits<uint32_t>::max()));
}

class TimingWindow {
public:
    void record(uint32_t elapsed_us) {
        const uint32_t index = writes_.load(std::memory_order_relaxed);
        values_[index % values_.size()].store(elapsed_us,
                                              std::memory_order_relaxed);
        uint32_t previous = maximum_.load(std::memory_order_relaxed);
        while (elapsed_us > previous &&
               !maximum_.compare_exchange_weak(previous, elapsed_us,
                                                std::memory_order_relaxed)) {
        }
        // Publish the completed sample only after both its slot and lifetime
        // maximum are visible to a concurrent stats() snapshot.
        writes_.store(index + 1U, std::memory_order_release);
    }

    TransportTimingSummary summary() const {
        TransportTimingSummary result;
        const uint32_t writes = writes_.load(std::memory_order_acquire);
        result.samples = writes;
        result.max_us = maximum_.load(std::memory_order_relaxed);
        const size_t count = std::min<size_t>(writes, values_.size());
        if (count == 0) {
            return result;
        }
        std::array<uint32_t, 64> ordered{};
        const uint32_t first = writes - static_cast<uint32_t>(count);
        for (size_t index = 0; index < count; ++index) {
            ordered[index] =
                values_[(first + index) % values_.size()].load(
                    std::memory_order_relaxed);
        }
        std::sort(ordered.begin(), ordered.begin() + count);
        result.p50_us = ordered[percentileIndex(count, 50)];
        result.p95_us = ordered[percentileIndex(count, 95)];
        return result;
    }

private:
    static size_t percentileIndex(size_t count, size_t percentile) {
        return ((count * percentile + 99U) / 100U) - 1U;
    }

    std::array<std::atomic<uint32_t>, 64> values_{};
    std::atomic<uint32_t> writes_{0};
    std::atomic<uint32_t> maximum_{0};
};

struct PayloadSlot {
    std::vector<uint8_t> bytes;

    void clear() { bytes.clear(); }
};

template <typename T>
class FixedSlotPool {
public:
    bool initialize(uint8_t capacity, size_t reserve_bytes) {
        reset();
        if (capacity == 0 || capacity > 32) {
            return false;
        }
        slots_.reset(new (std::nothrow) T[capacity]);
        if (!slots_) {
            return false;
        }
        capacity_ = capacity;
        for (uint8_t index = 0; index < capacity_; ++index) {
            slots_[index].bytes.reserve(reserve_bytes);
        }
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

enum class EventType : uint8_t {
    Open,
    Text,
    Binary,
    Reconnecting,
    Error,
};

struct EventMessage {
    EventType type = EventType::Open;
    uint32_t generation = 0;
    uint8_t payload_slot = kInvalidSlot;
};

struct TransmitMessage {
    uint32_t generation = 0;
    uint8_t payload_slot = kInvalidSlot;
};

}  // namespace

class AsyncTransport::Impl {
public:
    Impl(Transport& transport, const AsyncTransportConfig& config)
        : transport_(transport),
          config_(config),
          limits_{config.maximum_text_bytes, config.maximum_binary_bytes} {}

    ~Impl() {
        end(std::numeric_limits<uint32_t>::max());
    }

    void setCallbacks(TransportCallbacks callbacks) {
        callbacks_ = std::move(callbacks);
    }

    bool setLimits(const TransportLimits& limits) {
        if (limits.maximum_text_frame_bytes == 0 ||
            limits.maximum_binary_frame_bytes == 0 ||
            limits.maximum_text_frame_bytes > config_.maximum_text_bytes ||
            limits.maximum_binary_frame_bytes > config_.maximum_binary_bytes) {
            return false;
        }
        if (task_active_.load(std::memory_order_acquire)) {
            return limits.maximum_text_frame_bytes ==
                       limits_.maximum_text_frame_bytes &&
                   limits.maximum_binary_frame_bytes ==
                       limits_.maximum_binary_frame_bytes;
        }
        if (!transport_.setLimits(limits)) {
            return false;
        }
        limits_ = limits;
        limits_forwarded_ = true;
        return true;
    }

    bool connect(const TransportRequest& request) {
        if (request.url.empty() || !ensureStarted()) {
            return false;
        }
        if (xSemaphoreTake(request_mutex_, portMAX_DELAY) != pdTRUE) {
            return false;
        }
        desired_request_ = request;
        xSemaphoreGive(request_mutex_);

        const uint32_t generation =
            active_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        (void)generation;
        connected_.store(false, std::memory_order_release);
        desired_connected_.store(true, std::memory_order_release);
        discardQueuedEvents();
        discardQueuedTransmits();
        notifyWorker();
        return true;
    }

    void loop() {
        if (!task_active_.load(std::memory_order_acquire)) {
            return;
        }
        const uint32_t generation =
            active_generation_.load(std::memory_order_acquire);
        for (uint8_t index = 0; index < config_.maximum_events_per_loop;
             ++index) {
            EventMessage event;
            if (xQueueReceive(control_event_queue_, &event, 0) != pdTRUE &&
                xQueueReceive(audio_event_queue_, &event, 0) != pdTRUE) {
                break;
            }
            if (event.generation == generation) {
                const int64_t started_us = esp_timer_get_time();
                dispatchEvent(event);
                receive_dispatch_timing_.record(elapsedUs(started_us));
            }
            releaseEvent(event);
        }
    }

    bool sendText(const uint8_t* data, size_t size) {
        return enqueueTransmit(data, size, false);
    }

    bool sendBinary(const uint8_t* data, size_t size) {
        return enqueueTransmit(data, size, true);
    }

    void close() {
        desired_connected_.store(false, std::memory_order_release);
        connected_.store(false, std::memory_order_release);
        active_generation_.fetch_add(1, std::memory_order_acq_rel);
        if (request_mutex_ != nullptr &&
            xSemaphoreTake(request_mutex_, portMAX_DELAY) == pdTRUE) {
            TransportRequest empty;
            std::swap(desired_request_, empty);
            xSemaphoreGive(request_mutex_);
        }
        discardQueuedEvents();
        discardQueuedTransmits();
        notifyWorker();
    }

    bool connected() const {
        return connected_.load(std::memory_order_acquire);
    }

    void setEventNotifier(TransportEventNotifier notifier) {
        notifier_function_.store(nullptr, std::memory_order_release);
        notifier_context_.store(notifier.context, std::memory_order_relaxed);
        notifier_function_.store(notifier.notify, std::memory_order_release);
    }

    bool end(uint32_t timeout_ms) {
        if (!task_active_.load(std::memory_order_acquire)) {
            releaseResources();
            return true;
        }
        desired_connected_.store(false, std::memory_order_release);
        connected_.store(false, std::memory_order_release);
        stop_requested_.store(true, std::memory_order_release);
        notifyWorker();
        if (stopped_signal_ == nullptr ||
            xSemaphoreTake(stopped_signal_, timeoutTicks(timeout_ms)) != pdTRUE) {
            return false;
        }
        releaseResources();
        return true;
    }

    bool running() const {
        return task_active_.load(std::memory_order_acquire);
    }

    AsyncTransportStats stats() const {
        AsyncTransportStats result;
        result.connection_attempts = connection_attempts_.load();
        result.reconnect_attempts = reconnect_attempts_.load();
        result.connect_timeouts = connect_timeouts_.load();
        result.canceled_connects = canceled_connects_.load();
        result.control_messages_queued = control_messages_queued_.load();
        result.audio_messages_queued = audio_messages_queued_.load();
        result.control_messages_sent = control_messages_sent_.load();
        result.audio_messages_sent = audio_messages_sent_.load();
        result.receive_text_dropped = receive_text_dropped_.load();
        result.receive_binary_dropped = receive_binary_dropped_.load();
        result.transmit_control_rejected = transmit_control_rejected_.load();
        result.transmit_audio_rejected = transmit_audio_rejected_.load();
        result.frame_limits = combinedLimitStats();
        result.connect_timing = connect_timing_.summary();
        result.poll_timing = poll_timing_.summary();
        result.send_timing = send_timing_.summary();
        result.receive_dispatch_timing = receive_dispatch_timing_.summary();
        return result;
    }

    TransportLimitStats combinedLimitStats() const {
        TransportLimitStats result = transport_.limitStats();
        const uint32_t own_count = limit_violations_.load();
        result.violations += own_count;
        const uint64_t own_timestamp = latest_limit_timestamp_us_.load(
            std::memory_order_acquire);
        if (own_count != 0 && own_timestamp >= result.latest_timestamp_us) {
            result.latest_source = static_cast<TransportLimitSource>(
                latest_limit_source_.load());
            result.latest_type = static_cast<TransportPayloadType>(
                latest_limit_type_.load());
            result.latest_length = latest_limit_length_.load();
            result.latest_limit = latest_limit_value_.load();
            result.latest_timestamp_us = own_timestamp;
        }
        return result;
    }

private:
    Transport& transport_;
    AsyncTransportConfig config_;
    TransportLimits limits_;
    bool limits_forwarded_ = false;
    TransportCallbacks callbacks_;
    TransportRequest desired_request_;
    TransportRequest worker_request_;

    QueueHandle_t control_event_queue_ = nullptr;
    QueueHandle_t audio_event_queue_ = nullptr;
    QueueHandle_t control_tx_queue_ = nullptr;
    QueueHandle_t audio_tx_queue_ = nullptr;
    SemaphoreHandle_t request_mutex_ = nullptr;
    SemaphoreHandle_t stopped_signal_ = nullptr;
    std::atomic<TaskHandle_t> worker_task_{nullptr};

    FixedSlotPool<PayloadSlot> receive_text_pool_;
    FixedSlotPool<PayloadSlot> receive_binary_pool_;
    FixedSlotPool<PayloadSlot> receive_error_pool_;
    FixedSlotPool<PayloadSlot> transmit_control_pool_;
    FixedSlotPool<PayloadSlot> transmit_audio_pool_;

    std::atomic<bool> task_active_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> desired_connected_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> active_generation_{0};
    std::atomic<TransportEventNotifier::Notify> notifier_function_{nullptr};
    std::atomic<void*> notifier_context_{nullptr};

    bool worker_connected_ = false;
    bool worker_accept_receive_ = false;
    bool worker_expected_close_ = false;
    bool worker_connection_lost_ = false;
    bool worker_fatal_error_ = false;
    uint32_t worker_generation_ = 0;

    std::atomic<uint32_t> connection_attempts_{0};
    std::atomic<uint32_t> reconnect_attempts_{0};
    std::atomic<uint32_t> connect_timeouts_{0};
    std::atomic<uint32_t> canceled_connects_{0};
    std::atomic<uint32_t> control_messages_queued_{0};
    std::atomic<uint32_t> audio_messages_queued_{0};
    std::atomic<uint32_t> control_messages_sent_{0};
    std::atomic<uint32_t> audio_messages_sent_{0};
    std::atomic<uint32_t> receive_text_dropped_{0};
    std::atomic<uint32_t> receive_binary_dropped_{0};
    std::atomic<uint32_t> transmit_control_rejected_{0};
    std::atomic<uint32_t> transmit_audio_rejected_{0};
    std::atomic<uint32_t> limit_violations_{0};
    std::atomic<uint8_t> latest_limit_source_{
        static_cast<uint8_t>(TransportLimitSource::None)};
    std::atomic<uint8_t> latest_limit_type_{
        static_cast<uint8_t>(TransportPayloadType::None)};
    std::atomic<size_t> latest_limit_length_{0};
    std::atomic<size_t> latest_limit_value_{0};
    std::atomic<uint64_t> latest_limit_timestamp_us_{0};
    TimingWindow connect_timing_;
    TimingWindow poll_timing_;
    TimingWindow send_timing_;
    TimingWindow receive_dispatch_timing_;

    bool validConfig() const {
        return config_.task_stack_size >= 4096 && config_.task_priority > 0 &&
               config_.task_priority < configMAX_PRIORITIES &&
               config_.task_core >= -1 && config_.task_core < portNUM_PROCESSORS &&
               config_.poll_interval_ms > 0 &&
               config_.connect_timeout_ms > 0 &&
               config_.reconnect_initial_delay_ms > 0 &&
               config_.reconnect_initial_delay_ms <=
                   config_.reconnect_max_delay_ms &&
               config_.control_event_queue_depth >=
                   config_.receive_text_pool_depth +
                       config_.receive_error_pool_depth + 2 &&
               config_.audio_event_queue_depth > 0 &&
               config_.receive_text_pool_depth > 0 &&
               config_.receive_text_pool_depth <= 32 &&
               config_.receive_binary_pool_depth > 0 &&
               config_.receive_binary_pool_depth <= 32 &&
               config_.receive_error_pool_depth > 0 &&
               config_.receive_error_pool_depth <= 32 &&
               config_.transmit_control_pool_depth > 0 &&
               config_.transmit_control_pool_depth <= 32 &&
               config_.transmit_audio_pool_depth > 0 &&
               config_.transmit_audio_pool_depth <= 32 &&
               config_.maximum_control_sends_per_cycle > 0 &&
               config_.maximum_events_per_loop > 0 &&
               config_.maximum_text_bytes > 0 &&
               config_.maximum_text_bytes <= 16384 &&
               config_.maximum_binary_bytes > 0 &&
               config_.maximum_binary_bytes <= 16384 &&
               config_.maximum_error_bytes > 0 &&
               config_.maximum_error_bytes <= 2048;
    }

    bool ensureStarted() {
        if (task_active_.load(std::memory_order_acquire)) {
            return true;
        }
        if (!validConfig()) {
            return false;
        }
        if (!limits_forwarded_) {
            if (!transport_.setLimits(limits_)) {
                return false;
            }
            limits_forwarded_ = true;
        }
        releaseResources();
        control_event_queue_ = xQueueCreate(config_.control_event_queue_depth,
                                             sizeof(EventMessage));
        audio_event_queue_ = xQueueCreate(config_.audio_event_queue_depth,
                                           sizeof(EventMessage));
        control_tx_queue_ = xQueueCreate(config_.transmit_control_pool_depth,
                                          sizeof(TransmitMessage));
        audio_tx_queue_ = xQueueCreate(config_.transmit_audio_pool_depth,
                                        sizeof(TransmitMessage));
        request_mutex_ = xSemaphoreCreateMutex();
        stopped_signal_ = xSemaphoreCreateBinary();
        if (control_event_queue_ == nullptr || audio_event_queue_ == nullptr ||
            control_tx_queue_ == nullptr || audio_tx_queue_ == nullptr ||
            request_mutex_ == nullptr || stopped_signal_ == nullptr ||
            !receive_text_pool_.initialize(config_.receive_text_pool_depth,
                                           limits_.maximum_text_frame_bytes) ||
            !receive_binary_pool_.initialize(config_.receive_binary_pool_depth,
                                             limits_.maximum_binary_frame_bytes) ||
            !receive_error_pool_.initialize(config_.receive_error_pool_depth,
                                            config_.maximum_error_bytes) ||
            !transmit_control_pool_.initialize(
                config_.transmit_control_pool_depth,
                limits_.maximum_text_frame_bytes) ||
            !transmit_audio_pool_.initialize(config_.transmit_audio_pool_depth,
                                             limits_.maximum_binary_frame_bytes)) {
            releaseResources();
            return false;
        }

        stop_requested_.store(false);
        task_active_.store(true, std::memory_order_release);
        BaseType_t created = pdFAIL;
        TaskHandle_t task = nullptr;
#if defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
        if (config_.task_stack_in_psram) {
            created = xTaskCreatePinnedToCoreWithCaps(
                workerTaskEntry, "xiaozhi-net", config_.task_stack_size, this,
                config_.task_priority, &task,
                config_.task_core < 0 ? tskNO_AFFINITY : config_.task_core,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        } else
#endif
        if (config_.task_core < 0) {
            created = xTaskCreate(workerTaskEntry, "xiaozhi-net",
                                  config_.task_stack_size, this,
                                  config_.task_priority, &task);
        } else {
            created = xTaskCreatePinnedToCore(
                workerTaskEntry, "xiaozhi-net", config_.task_stack_size, this,
                config_.task_priority, &task, config_.task_core);
        }
        worker_task_.store(task, std::memory_order_release);
        if (created != pdPASS) {
            task_active_.store(false);
            worker_task_.store(nullptr, std::memory_order_release);
            releaseResources();
            return false;
        }
        return true;
    }

    void releaseResources() {
        if (task_active_.load(std::memory_order_acquire)) {
            return;
        }
        discardQueuedEvents();
        discardQueuedTransmits();
        if (control_event_queue_ != nullptr) {
            vQueueDelete(control_event_queue_);
            control_event_queue_ = nullptr;
        }
        if (audio_event_queue_ != nullptr) {
            vQueueDelete(audio_event_queue_);
            audio_event_queue_ = nullptr;
        }
        if (control_tx_queue_ != nullptr) {
            vQueueDelete(control_tx_queue_);
            control_tx_queue_ = nullptr;
        }
        if (audio_tx_queue_ != nullptr) {
            vQueueDelete(audio_tx_queue_);
            audio_tx_queue_ = nullptr;
        }
        if (request_mutex_ != nullptr) {
            vSemaphoreDelete(request_mutex_);
            request_mutex_ = nullptr;
        }
        if (stopped_signal_ != nullptr) {
            vSemaphoreDelete(stopped_signal_);
            stopped_signal_ = nullptr;
        }
        receive_text_pool_.reset();
        receive_binary_pool_.reset();
        receive_error_pool_.reset();
        transmit_control_pool_.reset();
        transmit_audio_pool_.reset();
        worker_task_.store(nullptr, std::memory_order_release);
    }

    static void workerTaskEntry(void* argument) {
        static_cast<Impl*>(argument)->workerTask();
    }

    void workerTask() {
        TransportCallbacks callbacks;
        callbacks.on_open = []() {};
        callbacks.on_text = [this](const uint8_t* data, size_t size) {
            if (!worker_accept_receive_ ||
                worker_generation_ != active_generation_.load()) {
                return;
            }
            if (size > limits_.maximum_text_frame_bytes ||
                !enqueuePayloadEvent(EventType::Text, worker_generation_, data,
                                     size, receive_text_pool_,
                                     control_event_queue_)) {
                ++receive_text_dropped_;
                if (size > limits_.maximum_text_frame_bytes) {
                    recordLimitViolation(TransportLimitSource::Receive,
                                         TransportPayloadType::Text, size,
                                         limits_.maximum_text_frame_bytes);
                }
                enqueueError(worker_generation_,
                             size > limits_.maximum_text_frame_bytes
                                 ? "asynchronous receive text frame exceeds its limit"
                                 : "asynchronous text receive pool is full");
                worker_fatal_error_ = true;
                worker_connection_lost_ = true;
            }
        };
        callbacks.on_binary = [this](const uint8_t* data, size_t size) {
            if (!worker_accept_receive_ ||
                worker_generation_ != active_generation_.load()) {
                return;
            }
            if (size > limits_.maximum_binary_frame_bytes ||
                !enqueuePayloadEvent(EventType::Binary, worker_generation_, data,
                                     size, receive_binary_pool_,
                                     audio_event_queue_)) {
                ++receive_binary_dropped_;
                if (size > limits_.maximum_binary_frame_bytes) {
                    recordLimitViolation(TransportLimitSource::Receive,
                                         TransportPayloadType::Binary, size,
                                         limits_.maximum_binary_frame_bytes);
                    enqueueError(worker_generation_,
                                 "asynchronous binary receive exceeds its limit");
                    worker_fatal_error_ = true;
                    worker_connection_lost_ = true;
                }
            }
        };
        callbacks.on_close = [this]() {
            if (!worker_expected_close_ && worker_connected_) {
                worker_connection_lost_ = true;
            }
        };
        callbacks.on_error = [this](const std::string& message) {
            // Synchronous transports can report an error from inside
            // connect(), before worker_connected_ becomes true. Dropping that
            // callback leaves Client in Connecting while the worker retries
            // forever, with no useful error or timeout visible to the caller.
            if (!desired_connected_.load(std::memory_order_acquire) ||
                worker_generation_ !=
                    active_generation_.load(std::memory_order_acquire)) {
                return;
            }
            enqueueError(worker_generation_, message);
            worker_fatal_error_ = true;
            worker_connection_lost_ = true;
        };
        transport_.setCallbacks(std::move(callbacks));

        uint32_t request_generation = 0;
        uint32_t attempts_for_generation = 0;
        uint32_t reconnect_delay_ms = config_.reconnect_initial_delay_ms;
        while (!stop_requested_.load(std::memory_order_acquire)) {
            if (!desired_connected_.load(std::memory_order_acquire)) {
                closeWorkerConnection();
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                continue;
            }

            const uint32_t desired_generation =
                active_generation_.load(std::memory_order_acquire);
            if (!worker_connected_ || worker_generation_ != desired_generation) {
                closeWorkerConnection();
                if (request_generation != desired_generation) {
                    if (!snapshotRequest(desired_generation)) {
                        taskYIELD();
                        continue;
                    }
                    request_generation = desired_generation;
                    attempts_for_generation = 0;
                    reconnect_delay_ms = config_.reconnect_initial_delay_ms;
                }

                ++attempts_for_generation;
                ++connection_attempts_;
                if (attempts_for_generation > 1) {
                    ++reconnect_attempts_;
                }
                worker_generation_ = desired_generation;
                worker_connection_lost_ = false;
                worker_fatal_error_ = false;
                const int64_t started_us = esp_timer_get_time();
                const bool opened = transport_.connect(worker_request_);
                const uint32_t elapsed_us = elapsedUs(started_us);
                connect_timing_.record(elapsed_us);
                const bool timed_out =
                    elapsed_us > config_.connect_timeout_ms * 1000ULL;
                const bool canceled = stop_requested_.load() ||
                                      !desired_connected_.load() ||
                                      active_generation_.load() != desired_generation;
                if (timed_out) {
                    ++connect_timeouts_;
                }
                if (canceled) {
                    ++canceled_connects_;
                }
                if (!opened || !transport_.connected() || timed_out || canceled) {
                    closeWorkerConnection();
                    if (!canceled) {
                        waitForRetry(reconnect_delay_ms);
                        reconnect_delay_ms = std::min<uint32_t>(
                            config_.reconnect_max_delay_ms,
                            reconnect_delay_ms * 2U);
                    }
                    continue;
                }

                worker_connected_ = true;
                worker_accept_receive_ = true;
                connected_.store(true, std::memory_order_release);
                reconnect_delay_ms = config_.reconnect_initial_delay_ms;
                if (!enqueueControlEvent(EventType::Open,
                                         desired_generation, true)) {
                    worker_connection_lost_ = true;
                }
            }

            bool send_failed = false;
            serviceTransmits(send_failed);
            if (!send_failed && worker_connected_) {
                const int64_t started_us = esp_timer_get_time();
                transport_.loop();
                poll_timing_.record(elapsedUs(started_us));
            }
            if (send_failed || worker_connection_lost_ ||
                !transport_.connected()) {
                const uint32_t lost_generation = worker_generation_;
                const bool fatal = worker_fatal_error_;
                closeWorkerConnection();
                if (active_generation_.load() == lost_generation &&
                    desired_connected_.load()) {
                    if (fatal) {
                        desired_connected_.store(false,
                                                 std::memory_order_release);
                    } else {
                        uint32_t expected_generation = lost_generation;
                        const uint32_t reconnect_generation =
                            lost_generation + 1U;
                        if (!active_generation_.compare_exchange_strong(
                                expected_generation, reconnect_generation,
                                std::memory_order_acq_rel)) {
                            continue;
                        }
                        // No payload or protocol send from the old WebSocket
                        // generation may cross the reconnect boundary.
                        discardQueuedEvents();
                        discardQueuedTransmits();
                        ++reconnect_attempts_;
                        enqueueControlEvent(EventType::Reconnecting,
                                            reconnect_generation, true);
                        waitForRetry(reconnect_delay_ms);
                        reconnect_delay_ms = std::min<uint32_t>(
                            config_.reconnect_max_delay_ms,
                            reconnect_delay_ms * 2U);
                    }
                }
                continue;
            }

            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(config_.poll_interval_ms));
        }

        closeWorkerConnection();
        transport_.setCallbacks({});
        worker_request_ = {};
        task_active_.store(false, std::memory_order_release);
        worker_task_.store(nullptr, std::memory_order_release);
        xSemaphoreGive(stopped_signal_);
#if defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
        if (config_.task_stack_in_psram) {
            vTaskDeleteWithCaps(nullptr);
        } else
#endif
        vTaskDelete(nullptr);
    }

    bool snapshotRequest(uint32_t generation) {
        if (xSemaphoreTake(request_mutex_, portMAX_DELAY) != pdTRUE) {
            return false;
        }
        const bool current = desired_connected_.load() &&
                             active_generation_.load() == generation;
        if (current) {
            worker_request_ = desired_request_;
        }
        xSemaphoreGive(request_mutex_);
        return current;
    }

    void closeWorkerConnection() {
        worker_accept_receive_ = false;
        connected_.store(false, std::memory_order_release);
        worker_expected_close_ = true;
        transport_.close();
        worker_expected_close_ = false;
        worker_connected_ = false;
        worker_connection_lost_ = false;
        worker_fatal_error_ = false;
    }

    void waitForRetry(uint32_t delay_ms) {
        if (stop_requested_.load() || !desired_connected_.load()) {
            return;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }

    void serviceTransmits(bool& failed) {
        failed = false;
        for (uint8_t index = 0;
             index < config_.maximum_control_sends_per_cycle; ++index) {
            TransmitMessage message;
            if (xQueueReceive(control_tx_queue_, &message, 0) != pdTRUE) {
                break;
            }
            if (message.generation == worker_generation_ && worker_connected_) {
                PayloadSlot& slot = transmit_control_pool_.at(message.payload_slot);
                const int64_t started_us = esp_timer_get_time();
                const bool sent =
                    transport_.sendText(slot.bytes.data(), slot.bytes.size());
                send_timing_.record(elapsedUs(started_us));
                if (!sent) {
                    failed = true;
                } else {
                    ++control_messages_sent_;
                }
            }
            transmit_control_pool_.release(message.payload_slot);
            if (failed) {
                return;
            }
        }

        TransmitMessage audio;
        if (xQueueReceive(audio_tx_queue_, &audio, 0) == pdTRUE) {
            if (audio.generation == worker_generation_ && worker_connected_) {
                PayloadSlot& slot = transmit_audio_pool_.at(audio.payload_slot);
                const int64_t started_us = esp_timer_get_time();
                const bool sent =
                    transport_.sendBinary(slot.bytes.data(), slot.bytes.size());
                send_timing_.record(elapsedUs(started_us));
                if (!sent) {
                    failed = true;
                } else {
                    ++audio_messages_sent_;
                }
            }
            transmit_audio_pool_.release(audio.payload_slot);
        }
    }

    bool enqueueTransmit(const uint8_t* data, size_t size, bool binary) {
        const bool invalid = data == nullptr || size == 0 ||
                             !task_active_.load() || !connected();
        const size_t maximum =
            binary ? limits_.maximum_binary_frame_bytes
                   : limits_.maximum_text_frame_bytes;
        if (invalid || size > maximum) {
            if (!invalid && size > maximum) {
                recordLimitViolation(TransportLimitSource::Transmit,
                                     binary ? TransportPayloadType::Binary
                                            : TransportPayloadType::Text,
                                     size, maximum);
            }
            if (binary) {
                ++transmit_audio_rejected_;
            } else {
                ++transmit_control_rejected_;
            }
            return false;
        }
        FixedSlotPool<PayloadSlot>& pool =
            binary ? transmit_audio_pool_ : transmit_control_pool_;
        QueueHandle_t queue = binary ? audio_tx_queue_ : control_tx_queue_;
        uint8_t slot_index = kInvalidSlot;
        PayloadSlot* slot = pool.acquire(slot_index);
        if (slot == nullptr) {
            if (binary) {
                ++transmit_audio_rejected_;
            } else {
                ++transmit_control_rejected_;
            }
            return false;
        }
        slot->bytes.assign(data, data + size);
        TransmitMessage message;
        message.generation = active_generation_.load(std::memory_order_acquire);
        message.payload_slot = slot_index;
        if (xQueueSend(queue, &message, 0) != pdTRUE) {
            pool.release(slot_index);
            if (binary) {
                ++transmit_audio_rejected_;
            } else {
                ++transmit_control_rejected_;
            }
            return false;
        }
        if (binary) {
            ++audio_messages_queued_;
        } else {
            ++control_messages_queued_;
        }
        notifyWorker();
        return true;
    }

    bool enqueuePayloadEvent(EventType type, uint32_t generation,
                             const uint8_t* data, size_t size,
                             FixedSlotPool<PayloadSlot>& pool,
                             QueueHandle_t queue) {
        if (data == nullptr || size == 0) {
            return false;
        }
        uint8_t slot_index = kInvalidSlot;
        PayloadSlot* slot = pool.acquire(slot_index);
        if (slot == nullptr) {
            return false;
        }
        slot->bytes.assign(data, data + size);
        EventMessage event;
        event.type = type;
        event.generation = generation;
        event.payload_slot = slot_index;
        if (xQueueSend(queue, &event, 0) != pdTRUE) {
            pool.release(slot_index);
            return false;
        }
        notifyClient();
        return true;
    }

    bool enqueueControlEvent(EventType type, uint32_t generation,
                             bool critical) {
        EventMessage event;
        event.type = type;
        event.generation = generation;
        if (xQueueSend(control_event_queue_, &event, 0) == pdTRUE) {
            notifyClient();
            return true;
        }
        if (!critical) {
            return false;
        }
        EventMessage displaced;
        if (xQueueReceive(control_event_queue_, &displaced, 0) == pdTRUE) {
            releaseEvent(displaced);
        }
        if (xQueueSend(control_event_queue_, &event, 0) != pdTRUE) {
            return false;
        }
        notifyClient();
        return true;
    }

    void enqueueError(uint32_t generation, const char* message) {
        if (message == nullptr) {
            return;
        }
        enqueueError(generation, message, std::strlen(message));
    }

    void enqueueError(uint32_t generation, const std::string& message) {
        enqueueError(generation, message.data(), message.size());
    }

    void enqueueError(uint32_t generation, const char* message, size_t length) {
        uint8_t slot_index = kInvalidSlot;
        PayloadSlot* slot = receive_error_pool_.acquire(slot_index);
        if (slot == nullptr) {
            return;
        }
        const size_t size = std::min(length, config_.maximum_error_bytes);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(message);
        slot->bytes.assign(bytes, bytes + size);
        EventMessage event;
        event.type = EventType::Error;
        event.generation = generation;
        event.payload_slot = slot_index;
        if (xQueueSend(control_event_queue_, &event, 0) != pdTRUE) {
            EventMessage displaced;
            if (xQueueReceive(control_event_queue_, &displaced, 0) == pdTRUE) {
                releaseEvent(displaced);
            }
            if (xQueueSend(control_event_queue_, &event, 0) != pdTRUE) {
                receive_error_pool_.release(slot_index);
                return;
            }
        }
        notifyClient();
    }

    void recordLimitViolation(TransportLimitSource source,
                              TransportPayloadType type, size_t length,
                              size_t limit) {
        latest_limit_source_.store(static_cast<uint8_t>(source));
        latest_limit_type_.store(static_cast<uint8_t>(type));
        latest_limit_length_.store(length);
        latest_limit_value_.store(limit);
        latest_limit_timestamp_us_.store(
            static_cast<uint64_t>(esp_timer_get_time()),
            std::memory_order_release);
        limit_violations_.fetch_add(1);
    }

    void dispatchEvent(const EventMessage& event) {
        switch (event.type) {
            case EventType::Open:
                if (connected() && callbacks_.on_open) {
                    callbacks_.on_open();
                }
                break;
            case EventType::Text:
                if (callbacks_.on_text) {
                    const PayloadSlot& slot =
                        receive_text_pool_.at(event.payload_slot);
                    callbacks_.on_text(slot.bytes.data(), slot.bytes.size());
                }
                break;
            case EventType::Binary:
                if (callbacks_.on_binary) {
                    const PayloadSlot& slot =
                        receive_binary_pool_.at(event.payload_slot);
                    callbacks_.on_binary(slot.bytes.data(), slot.bytes.size());
                }
                break;
            case EventType::Reconnecting:
                if (callbacks_.on_reconnecting) {
                    callbacks_.on_reconnecting();
                }
                break;
            case EventType::Error:
                if (callbacks_.on_error) {
                    const PayloadSlot& slot =
                        receive_error_pool_.at(event.payload_slot);
                    const std::string message(
                        reinterpret_cast<const char*>(slot.bytes.data()),
                        slot.bytes.size());
                    callbacks_.on_error(message);
                }
                break;
        }
    }

    void releaseEvent(const EventMessage& event) {
        if (event.payload_slot == kInvalidSlot) {
            return;
        }
        switch (event.type) {
            case EventType::Text:
                receive_text_pool_.release(event.payload_slot);
                break;
            case EventType::Binary:
                receive_binary_pool_.release(event.payload_slot);
                break;
            case EventType::Error:
                receive_error_pool_.release(event.payload_slot);
                break;
            case EventType::Open:
            case EventType::Reconnecting:
                break;
        }
    }

    void discardQueuedEvents() {
        EventMessage event;
        if (control_event_queue_ != nullptr) {
            while (xQueueReceive(control_event_queue_, &event, 0) == pdTRUE) {
                releaseEvent(event);
            }
        }
        if (audio_event_queue_ != nullptr) {
            while (xQueueReceive(audio_event_queue_, &event, 0) == pdTRUE) {
                releaseEvent(event);
            }
        }
    }

    void discardQueuedTransmits() {
        TransmitMessage message;
        if (control_tx_queue_ != nullptr) {
            while (xQueueReceive(control_tx_queue_, &message, 0) == pdTRUE) {
                transmit_control_pool_.release(message.payload_slot);
            }
        }
        if (audio_tx_queue_ != nullptr) {
            while (xQueueReceive(audio_tx_queue_, &message, 0) == pdTRUE) {
                transmit_audio_pool_.release(message.payload_slot);
            }
        }
    }

    void notifyWorker() {
        TaskHandle_t task = worker_task_.load(std::memory_order_acquire);
        if (task != nullptr) {
            xTaskNotifyGive(task);
        }
    }

    void notifyClient() {
        const TransportEventNotifier::Notify notify =
            notifier_function_.load(std::memory_order_acquire);
        if (notify != nullptr) {
            notify(notifier_context_.load(std::memory_order_relaxed));
        }
    }
};

AsyncTransport::AsyncTransport(Transport& transport,
                               const AsyncTransportConfig& config)
    : impl_(std::make_unique<Impl>(transport, config)) {}

AsyncTransport::~AsyncTransport() = default;

void AsyncTransport::setCallbacks(TransportCallbacks callbacks) {
    impl_->setCallbacks(std::move(callbacks));
}

bool AsyncTransport::setLimits(const TransportLimits& limits) {
    return impl_->setLimits(limits);
}

bool AsyncTransport::connect(const TransportRequest& request) {
    return impl_->connect(request);
}

void AsyncTransport::loop() {
    impl_->loop();
}

bool AsyncTransport::sendText(const uint8_t* data, size_t size) {
    return impl_->sendText(data, size);
}

bool AsyncTransport::sendBinary(const uint8_t* data, size_t size) {
    return impl_->sendBinary(data, size);
}

void AsyncTransport::close() {
    impl_->close();
}

bool AsyncTransport::connected() const {
    return impl_->connected();
}

void AsyncTransport::setEventNotifier(TransportEventNotifier notifier) {
    impl_->setEventNotifier(notifier);
}

TransportLimitStats AsyncTransport::limitStats() const {
    return impl_->combinedLimitStats();
}

bool AsyncTransport::end(uint32_t timeout_ms) {
    return impl_->end(timeout_ms);
}

bool AsyncTransport::running() const {
    return impl_->running();
}

AsyncTransportStats AsyncTransport::stats() const {
    return impl_->stats();
}

}  // namespace xiaozhi
