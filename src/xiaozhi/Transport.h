#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace xiaozhi {

struct TransportRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
};

// Wire-frame limits installed by Client before begin(). Binary includes the
// protocol v2/v3 header, not only the Opus payload.
struct TransportLimits {
    size_t maximum_text_frame_bytes = 8192;
    size_t maximum_binary_frame_bytes = 4112;
};

enum class TransportLimitSource : uint8_t {
    None,
    Receive,
    Transmit,
};

enum class TransportPayloadType : uint8_t {
    None,
    Text,
    Binary,
};

// Oversized frames are exceptional, so retaining the latest exact violation
// plus a total count is sufficient without allocating a log record per frame.
struct TransportLimitStats {
    uint32_t violations = 0;
    TransportLimitSource latest_source = TransportLimitSource::None;
    TransportPayloadType latest_type = TransportPayloadType::None;
    size_t latest_length = 0;
    size_t latest_limit = 0;
    uint64_t latest_timestamp_us = 0;
};

// P50/P95 cover the latest bounded timing window; max_us is lifetime maximum.
struct TransportTimingSummary {
    uint32_t samples = 0;
    uint32_t p50_us = 0;
    uint32_t p95_us = 0;
    uint32_t max_us = 0;
};

struct TransportCallbacks {
    std::function<void()> on_open;
    std::function<void(const uint8_t* data, size_t size)> on_text;
    std::function<void(const uint8_t* data, size_t size)> on_binary;
    std::function<void()> on_close;
    std::function<void(const std::string& message)> on_error;
    // An asynchronous transport emits this when a socket generation is lost
    // but desired-connected state remains true and retry will continue.
    std::function<void()> on_reconnecting;
};

// Cross-task notification used by asynchronous transports. The callback must
// be non-blocking and must not call Transport or Client directly.
struct TransportEventNotifier {
    using Notify = void (*)(void* context);

    Notify notify = nullptr;
    void* context = nullptr;
};

class Transport {
public:
    virtual ~Transport() = default;

    // A synchronous implementation dispatches callbacks on the Client task.
    // An implementation that returns asynchronous()==true owns the cross-task
    // copying and dispatches queued callbacks only when Client calls loop().
    virtual void setCallbacks(TransportCallbacks callbacks) = 0;
    // Called before Client starts. Implementations with fixed buffers may
    // reject limits they cannot safely enforce. The compatibility default
    // leaves enforcement to Client at the parse/send boundaries.
    virtual bool setLimits(const TransportLimits& limits) {
        (void)limits;
        return true;
    }
    // For a synchronous transport, true means the connection is usable and
    // on_open was dispatched before return. For an asynchronous transport, true
    // means bounded connect work was accepted and on_open arrives later via
    // loop(). false never permits a late callback from that rejected request.
    virtual bool connect(const TransportRequest& request) = 0;
    virtual void loop() = 0;
    // send* must consume or copy data before returning and must not retain the
    // pointer. For an asynchronous transport, true means copied into bounded TX
    // ownership, not that the socket write has completed.
    virtual bool sendText(const uint8_t* data, size_t size) = 0;
    virtual bool sendBinary(const uint8_t* data, size_t size) = 0;
    // close() is idempotent and is a callback-generation barrier even when
    // connected() is already false. It must not expose a late callback from the
    // old connection after a later connect() begins. A synchronous transport
    // quiesces the socket before returning; an async wrapper may let its worker
    // unwind a blocking call later, but must discard that old-generation result.
    virtual void close() = 0;
    virtual bool connected() const = 0;

    // Async implementations accept connect/send work into bounded queues and
    // dispatch their callbacks later from loop(). Synchronous custom transports
    // keep the original contract and need not override these hooks.
    virtual bool asynchronous() const { return false; }
    virtual void setEventNotifier(TransportEventNotifier notifier) {
        (void)notifier;
    }
    virtual TransportLimitStats limitStats() const { return {}; }
};

}  // namespace xiaozhi
