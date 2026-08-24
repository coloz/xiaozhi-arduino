#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "Transport.h"

namespace xiaozhi {

// Resource and scheduling bounds for AsyncTransport. Payload slots reserve
// their configured maximum once the worker starts, so queue pressure is
// handled by rejection/drop policy instead of unbounded network-task growth.
struct AsyncTransportConfig {
    uint32_t task_stack_size = 8192;
    // Keep this below ClientRuntime's default priority 2. Even a lower-level
    // connect implementation that spins instead of blocking remains preemptible
    // by local wake/abort/mute handling on the same core.
    uint8_t task_priority = 1;
    int8_t task_core = 0;
    uint16_t poll_interval_ms = 2;
    uint32_t connect_timeout_ms = 10000;
    uint16_t reconnect_initial_delay_ms = 250;
    uint16_t reconnect_max_delay_ms = 5000;

    uint8_t control_event_queue_depth = 6;
    uint8_t audio_event_queue_depth = 2;
    uint8_t receive_text_pool_depth = 2;
    uint8_t receive_binary_pool_depth = 2;
    uint8_t receive_error_pool_depth = 1;
    // Wake replacement can enqueue abort, detect, and listen control frames
    // while a socket send is stalled; one spare preserves that bounded chain.
    uint8_t transmit_control_pool_depth = 4;
    // Matches I2sOpusAudioPort's default four-packet drain batch. Further
    // backlog is rejected per packet without growing or closing the session.
    uint8_t transmit_audio_pool_depth = 4;
    uint8_t maximum_control_sends_per_cycle = 4;
    uint8_t maximum_events_per_loop = 4;

    // Capacity ceilings. Client installs its exact wire limits before begin;
    // begin fails if they exceed these preallocated pool capacities.
    size_t maximum_text_bytes = 8192;
    size_t maximum_binary_bytes = 4112;
    size_t maximum_error_bytes = 256;
    // ESP32 builds that enable external task stacks can keep this relatively
    // large network/TLS stack in PSRAM, preserving scarce internal SRAM for
    // mbedTLS handshake allocations. A board without PSRAM must leave it false.
    bool task_stack_in_psram = false;
};

struct AsyncTransportStats {
    uint32_t connection_attempts = 0;
    uint32_t reconnect_attempts = 0;
    uint32_t connect_timeouts = 0;
    uint32_t canceled_connects = 0;
    uint32_t control_messages_queued = 0;
    uint32_t audio_messages_queued = 0;
    uint32_t control_messages_sent = 0;
    uint32_t audio_messages_sent = 0;
    uint32_t receive_text_dropped = 0;
    uint32_t receive_binary_dropped = 0;
    uint32_t transmit_control_rejected = 0;
    uint32_t transmit_audio_rejected = 0;
    TransportLimitStats frame_limits;
    // Rolling percentiles use the latest 64 samples; max is lifetime.
    TransportTimingSummary connect_timing;
    TransportTimingSummary poll_timing;
    TransportTimingSummary send_timing;
    TransportTimingSummary receive_dispatch_timing;
};

// Runs an existing synchronous Transport on a dedicated network task. Client
// sees only non-blocking queue operations and receives copied events when it
// calls loop(). The wrapped transport must not be accessed anywhere else after
// AsyncTransport has started.
class AsyncTransport final : public Transport {
public:
    explicit AsyncTransport(Transport& transport,
                            const AsyncTransportConfig& config = {});
    ~AsyncTransport() override;

    AsyncTransport(const AsyncTransport&) = delete;
    AsyncTransport& operator=(const AsyncTransport&) = delete;

    void setCallbacks(TransportCallbacks callbacks) override;
    bool setLimits(const TransportLimits& limits) override;
    // true means the desired connection request was accepted. on_open is
    // dispatched later from loop() after the network task has connected.
    bool connect(const TransportRequest& request) override;
    void loop() override;
    // true means the payload was copied into its bounded TX pool. Actual socket
    // I/O happens later on the network task.
    bool sendText(const uint8_t* data, size_t size) override;
    bool sendBinary(const uint8_t* data, size_t size) override;
    // Publishes disconnected desired state immediately. A generation barrier
    // prevents callbacks or queued TX from the old connection being delivered.
    void close() override;
    bool connected() const override;
    bool asynchronous() const override { return true; }
    void setEventNotifier(TransportEventNotifier notifier) override;
    TransportLimitStats limitStats() const override;

    // Stops the network task and quiesces the wrapped transport. A finite
    // timeout that returns false leaves the object active and safe to retry.
    bool end(uint32_t timeout_ms = 5000);
    bool running() const;
    AsyncTransportStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xiaozhi
