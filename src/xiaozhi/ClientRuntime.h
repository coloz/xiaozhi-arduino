#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "Client.h"

namespace xiaozhi {

// Runs the single-task Client API on a dedicated FreeRTOS task. This keeps
// WebSocket polling, audio uplink, and protocol timeouts independent from a
// slow Arduino loop(). User callbacks are queued and dispatched by loop(), so
// display or application callbacks cannot stall the protocol task either.
struct ClientRuntimeConfig {
    uint32_t task_stack_size = 8192;
    uint8_t task_priority = 2;
    // Pinning to core 0 isolates the runtime from Arduino's usual loopTask on
    // core 1. Set to -1 to let FreeRTOS choose a core.
    int8_t task_core = 0;
    // Active sessions are polled aggressively; idle waits are longer but a
    // queued command wakes the task immediately.
    uint16_t poll_interval_ms = 2;
    uint16_t idle_poll_interval_ms = 20;
    uint8_t command_queue_depth = 8;
    uint8_t callback_queue_depth = 12;
    uint8_t maximum_commands_per_cycle = 4;
};

struct ClientRuntimeStats {
    uint32_t service_cycles = 0;
    uint32_t commands_queued = 0;
    uint32_t commands_executed = 0;
    uint32_t commands_rejected = 0;
    uint32_t callbacks_queued = 0;
    uint32_t callbacks_dispatched = 0;
    uint32_t callbacks_dropped = 0;
    uint8_t command_queue_high_watermark = 0;
    uint8_t callback_queue_high_watermark = 0;
};

class ClientRuntime {
public:
    explicit ClientRuntime(Client& client);
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    // Client configuration such as attachAudioPort() and MCP tool registration
    // must be completed before begin(). Do not call Client methods directly
    // while this runtime is active.
    bool begin(const ClientConfig& client_config, Callbacks callbacks = {},
               const ClientRuntimeConfig& runtime_config = {});
    // Waits for the service task to leave Client::loop() and complete teardown.
    // UINT32_MAX waits indefinitely. A timeout leaves the runtime active so the
    // caller may retry safely without freeing objects still used by the task.
    bool end(uint32_t timeout_ms = 5000);

    // Dispatch at most max_callbacks user callbacks on the Arduino task. Call
    // this regularly for UI/events; protocol and audio servicing do not depend
    // on it. A zero limit drains every callback currently queued.
    size_t loop(size_t max_callbacks = 4);

    // Request methods are non-blocking and safe to call from the Arduino task
    // or other application tasks. true means accepted into the bounded command
    // queue; execution errors are reported through on_error.
    bool requestStartListening(ListeningMode mode = ListeningMode::ManualStop);
    bool requestStopListening();
    bool requestToggleChat();
    bool requestAbortSpeaking(AbortReason reason = AbortReason::None);
    bool requestWakeWordDetected(const std::string& wake_word);
    bool requestCloseSession();
    bool requestSendMcp(const std::string& json_rpc_payload);

    bool running() const;
    bool ready() const;
    bool sessionReady() const;
    State state() const;
    const char* stateName() const { return xiaozhi::stateName(state()); }
    ClientRuntimeStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xiaozhi
