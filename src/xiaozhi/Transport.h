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
};

class Transport {
public:
    virtual ~Transport() = default;

    // Client and Transport methods are single-task APIs. Implementations may
    // dispatch callbacks synchronously from connect/send/close or from loop(),
    // but always on that same task. Client defers protocol sends until an active
    // connect/loop/send method returns, so Transport methods need not support
    // recursive send calls.
    virtual void setCallbacks(TransportCallbacks callbacks) = 0;
    // connect() itself is synchronous: true means the connection is usable and
    // on_open has been dispatched before return; false means no callback from
    // that failed attempt may arrive later. Protocol hello remains asynchronous.
    virtual bool connect(const TransportRequest& request) = 0;
    virtual void loop() = 0;
    // send* must consume or copy data before returning and must not retain the
    // pointer. Client reuses its binary framing buffer on the next audio packet.
    virtual bool sendText(const uint8_t* data, size_t size) = 0;
    virtual bool sendBinary(const uint8_t* data, size_t size) = 0;
    // close() is idempotent and must quiesce the old connection before returning,
    // even when connected() is already false. It may dispatch one or more
    // synchronous terminal callbacks, but must not deliver a late callback from
    // that connection after a subsequent connect() begins. Client can request
    // close() from any such callback; an implementation may defer destruction
    // until its active method unwinds, as ArduinoWebSocketTransport does.
    virtual void close() = 0;
    virtual bool connected() const = 0;
};

}  // namespace xiaozhi
