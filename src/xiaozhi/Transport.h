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

    // Implementations dispatch callbacks from loop(), on the same thread that calls Client::loop().
    // This keeps sketch callbacks deterministic and avoids exposing a hidden RTOS concurrency model.
    virtual void setCallbacks(TransportCallbacks callbacks) = 0;
    virtual bool connect(const TransportRequest& request) = 0;
    virtual void loop() = 0;
    virtual bool sendText(const uint8_t* data, size_t size) = 0;
    virtual bool sendBinary(const uint8_t* data, size_t size) = 0;
    virtual void close() = 0;
    virtual bool connected() const = 0;
};

}  // namespace xiaozhi
