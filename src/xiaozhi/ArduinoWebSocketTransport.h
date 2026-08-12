#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "Transport.h"

namespace websockets {
class WebsocketsClient;
}

namespace xiaozhi {

class ArduinoWebSocketTransport final : public Transport {
public:
    ArduinoWebSocketTransport();
    ~ArduinoWebSocketTransport() override;

    ArduinoWebSocketTransport(const ArduinoWebSocketTransport&) = delete;
    ArduinoWebSocketTransport& operator=(const ArduinoWebSocketTransport&) = delete;

    void setCallbacks(TransportCallbacks callbacks) override;
    bool setLimits(const TransportLimits& limits) override;
    bool connect(const TransportRequest& request) override;
    void loop() override;
    bool sendText(const uint8_t* data, size_t size) override;
    bool sendBinary(const uint8_t* data, size_t size) override;
    void close() override;
    bool connected() const override { return connected_; }
    TransportLimitStats limitStats() const override;

    // The certificate memory must remain valid for the lifetime of a connection.
    void setCACertificate(const char* pem_certificate);

private:
    void beginClientCall();
    void endClientCall();
    void closeNow();
    void finishClientCall();
    void recordLimitViolation(TransportLimitSource source,
                              TransportPayloadType type, size_t length,
                              size_t limit);

    TransportCallbacks callbacks_;
    std::unique_ptr<websockets::WebsocketsClient> client_;
    const char* ca_certificate_ = nullptr;
    bool connected_ = false;
    size_t client_call_depth_ = 0;
    bool close_pending_ = false;
    bool suppress_close_callback_ = false;
    std::string fragment_buffer_;
    bool fragment_in_progress_ = false;
    bool fragment_is_text_ = false;
    bool fragment_release_pending_ = false;
    TransportLimits limits_;
    std::atomic<uint32_t> limit_violations_{0};
    std::atomic<uint8_t> latest_limit_source_{
        static_cast<uint8_t>(TransportLimitSource::None)};
    std::atomic<uint8_t> latest_limit_type_{
        static_cast<uint8_t>(TransportPayloadType::None)};
    std::atomic<size_t> latest_limit_length_{0};
    std::atomic<size_t> latest_limit_value_{0};
    std::atomic<uint64_t> latest_limit_timestamp_us_{0};
};

}  // namespace xiaozhi
