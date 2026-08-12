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
};

}  // namespace xiaozhi
