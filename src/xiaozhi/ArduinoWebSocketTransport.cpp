#include "ArduinoWebSocketTransport.h"

#include <Arduino.h>
#if __has_include(<ArduinoWebsockets.h>)
#include <ArduinoWebsockets.h>
#define XIAOZHI_HAS_ARDUINO_WEBSOCKETS 1
#else
#define XIAOZHI_HAS_ARDUINO_WEBSOCKETS 0
namespace websockets {
class WebsocketsClient {};
}  // namespace websockets
#endif

#include <utility>

namespace xiaozhi {

ArduinoWebSocketTransport::ArduinoWebSocketTransport() = default;

ArduinoWebSocketTransport::~ArduinoWebSocketTransport() {
    close();
}

void ArduinoWebSocketTransport::setCallbacks(TransportCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

bool ArduinoWebSocketTransport::connect(const TransportRequest& request) {
#if XIAOZHI_HAS_ARDUINO_WEBSOCKETS
    // Replacing the client while inside its own callback would destroy an object whose
    // connect/poll/send stack is still active. The caller can retry from the next loop().
    if (client_call_depth_ != 0) {
        return false;
    }
#endif
    close();
#if !XIAOZHI_HAS_ARDUINO_WEBSOCKETS
    (void)request;
    if (callbacks_.on_error) {
        callbacks_.on_error(
            "ArduinoWebsockets is not installed; install version 0.5.4 or newer");
    }
    return false;
#else
    if (request.url.empty()) {
        if (callbacks_.on_error) {
            callbacks_.on_error("WebSocket URL is empty");
        }
        return false;
    }

    const bool secure = request.url.rfind("wss://", 0) == 0;
    if (secure && ca_certificate_ == nullptr) {
        if (callbacks_.on_error) {
            callbacks_.on_error("WSS requires setCACertificate() with a trusted root CA");
        }
        return false;
    }

    client_ = std::make_unique<websockets::WebsocketsClient>();
    if (secure) {
        client_->setCACert(ca_certificate_);
    }
    for (const auto& header : request.headers) {
        client_->addHeader(String(header.first.c_str()), String(header.second.c_str()));
    }

    client_->onMessage([this](websockets::WebsocketsMessage message) {
        const std::string& payload = message.rawData();
        constexpr size_t kHardMessageLimit = 16384;
        if (payload.size() > kHardMessageLimit) {
            if (callbacks_.on_error) {
                callbacks_.on_error("WebSocket message exceeds the 16384-byte hard limit");
            }
            close();
            return;
        }
        const uint8_t* data = reinterpret_cast<const uint8_t*>(payload.data());
        if (message.isText()) {
            if (callbacks_.on_text) {
                callbacks_.on_text(data, payload.size());
            }
        } else if (message.isBinary() && callbacks_.on_binary) {
            callbacks_.on_binary(data, payload.size());
        }
    });
    client_->onEvent([this](websockets::WebsocketsEvent event, String data) {
        switch (event) {
            case websockets::WebsocketsEvent::ConnectionOpened:
                connected_ = true;
                if (callbacks_.on_open) {
                    callbacks_.on_open();
                }
                break;
            case websockets::WebsocketsEvent::ConnectionClosed:
                connected_ = false;
                if (!suppress_close_callback_ && callbacks_.on_close) {
                    callbacks_.on_close();
                }
                break;
            case websockets::WebsocketsEvent::GotPing:
            case websockets::WebsocketsEvent::GotPong:
                break;
        }
        (void)data;
    });

    beginClientCall();
    const bool opened = client_->connect(String(request.url.c_str()));
    endClientCall();
    if (!opened || !connected_) {
        connected_ = false;
        client_.reset();
        if (callbacks_.on_error) {
            callbacks_.on_error("WebSocket TCP/TLS handshake failed");
        }
        return false;
    }
    return connected_;
#endif
}

void ArduinoWebSocketTransport::loop() {
#if XIAOZHI_HAS_ARDUINO_WEBSOCKETS
    if (client_ != nullptr && connected_) {
        beginClientCall();
        client_->poll();
        endClientCall();
    }
#endif
}

bool ArduinoWebSocketTransport::sendText(const uint8_t* data, size_t size) {
#if XIAOZHI_HAS_ARDUINO_WEBSOCKETS
    if (client_ == nullptr || !connected_ || data == nullptr || size == 0) {
        return false;
    }
    beginClientCall();
    const bool sent = client_->send(reinterpret_cast<const char*>(data), size);
    endClientCall();
    return sent;
#else
    (void)data;
    (void)size;
    return false;
#endif
}

bool ArduinoWebSocketTransport::sendBinary(const uint8_t* data, size_t size) {
#if XIAOZHI_HAS_ARDUINO_WEBSOCKETS
    if (client_ == nullptr || !connected_ || data == nullptr || size == 0) {
        return false;
    }
    beginClientCall();
    const bool sent = client_->sendBinary(reinterpret_cast<const char*>(data), size);
    endClientCall();
    return sent;
#else
    (void)data;
    (void)size;
    return false;
#endif
}

void ArduinoWebSocketTransport::close() {
#if XIAOZHI_HAS_ARDUINO_WEBSOCKETS
    suppress_close_callback_ = true;
    connected_ = false;
    if (client_call_depth_ != 0) {
        close_pending_ = true;
        return;
    }
    closeNow();
#else
    client_.reset();
#endif
    connected_ = false;
}

void ArduinoWebSocketTransport::closeNow() {
#if XIAOZHI_HAS_ARDUINO_WEBSOCKETS
    close_pending_ = false;
    if (client_ != nullptr) {
        beginClientCall();
        client_->close();
        endClientCall();
        client_.reset();
    }
#else
    client_.reset();
#endif
    connected_ = false;
    suppress_close_callback_ = false;
}

void ArduinoWebSocketTransport::beginClientCall() {
    ++client_call_depth_;
}

void ArduinoWebSocketTransport::endClientCall() {
    if (client_call_depth_ != 0) {
        --client_call_depth_;
    }
    finishClientCall();
}

void ArduinoWebSocketTransport::finishClientCall() {
    if (client_call_depth_ == 0 && close_pending_) {
        closeNow();
    }
}

void ArduinoWebSocketTransport::setCACertificate(const char* pem_certificate) {
    ca_certificate_ = pem_certificate;
}

}  // namespace xiaozhi
