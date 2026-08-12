#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "Client.h"

namespace xiaozhi {

// Internal state-transition hook for bounded hardware policy such as Wi-Fi
// modem sleep. It runs synchronously on the Runtime service task after Client
// and its audio port have applied their lifecycle state, and before the UI
// observer is queued. The hook must not block, allocate, call Client/Runtime,
// or touch a display. Runtime measures the configured execution-time budget;
// an overrun is observable but cannot be preempted.
struct ClientRuntimeLifecycleHooks {
    using StateChanged = void (*)(void* context, State old_state, State new_state,
                                  bool session_ready);

    StateChanged on_state_changed = nullptr;
    void* context = nullptr;
    uint32_t maximum_execution_us = 5000;
};

// Detects a stalled TTS turn on the Runtime service task. A timeout is armed
// when Speaking begins, re-armed by each accepted downlink audio packet, and
// closes the stale session only after the playback port reports idle.
struct ClientRuntimePlaybackWatchdogConfig {
    bool enabled = true;
    uint32_t first_audio_timeout_ms = 12000;
    uint32_t inter_packet_timeout_ms = 12000;
};

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
    ClientRuntimeLifecycleHooks lifecycle;
    ClientRuntimePlaybackWatchdogConfig playback_watchdog;
};

struct ClientRuntimeStats {
    uint32_t service_cycles = 0;
    uint32_t commands_queued = 0;
    uint32_t commands_executed = 0;
    uint32_t commands_rejected = 0;
    uint32_t callbacks_queued = 0;
    uint32_t callbacks_dispatched = 0;
    uint32_t callbacks_dropped = 0;
    uint32_t wake_events_queued = 0;
    uint32_t wake_events_executed = 0;
    uint32_t wake_events_coalesced = 0;
    uint32_t wake_events_rejected = 0;
    uint32_t lifecycle_hook_calls = 0;
    uint32_t lifecycle_hook_overruns = 0;
    uint32_t lifecycle_hook_maximum_us = 0;
    uint32_t playback_first_audio_timeouts = 0;
    uint32_t playback_inter_packet_timeouts = 0;
    uint32_t playback_timeout_close_failures = 0;
    bool playback_idle = true;
    uint32_t urgent_controls_queued = 0;
    uint32_t urgent_controls_executed = 0;
    uint32_t urgent_controls_coalesced = 0;
    uint32_t urgent_controls_rejected = 0;
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
    // or other application tasks. Stop/abort/wake/mute use a fixed high-priority
    // channel and coalesce repeated intent; other requests use the normal queue.
    bool requestStartListening(ListeningMode mode = ListeningMode::ManualStop);
    bool requestStopListening();
    bool requestToggleChat();
    bool requestAbortSpeaking(AbortReason reason = AbortReason::None);
    bool requestWakeWordDetected(const std::string& wake_word);
    bool requestWakeWordDetected(const char* wake_word, size_t size);
    bool requestPlaybackMute(bool muted);
    bool requestCloseSession();
    bool requestSendMcp(const std::string& json_rpc_payload);

    // ISR variants only publish a bit or a fixed-size wake event. They never
    // allocate, wait, or invoke Client/audio code in interrupt context.
    bool requestStopListeningFromISR();
    bool requestAbortSpeakingFromISR(
        AbortReason reason = AbortReason::None);
    bool requestWakeWordDetectedFromISR(const char* wake_word, size_t size);
    bool requestPlaybackMuteFromISR(bool muted);

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
