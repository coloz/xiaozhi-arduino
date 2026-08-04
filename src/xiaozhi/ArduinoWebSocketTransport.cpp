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
    // Aggregate fragments ourselves and close as soon as the cumulative limit
    // is crossed. ArduinoWebsockets 0.5.4 still retains a second copy of the
    // first fragmented message because its policy setter does not rebuild the
    // existing StreamBuilder, but this callback bounds that copy as well.
    client_->setFragmentsPolicy(websockets::FragmentsPolicy_Notify);
    if (secure) {
        client_->setCACert(ca_certificate_);
    }
    for (const auto& header : request.headers) {
        client_->addHeader(String(header.first.c_str()), String(header.second.c_str()));
    }

    client_->onMessage([this](websockets::WebsocketsClient&,
                              websockets::WebsocketsMessage message) {
        if (!connected_ || close_pending_) {
            return;
        }
        const std::string& payload = message.rawData();
        constexpr size_t kHardMessageLimit = 16384;
        const auto rejectOversizedMessage = [this]() {
            if (callbacks_.on_error) {
                callbacks_.on_error("WebSocket message exceeds the 16384-byte hard limit");
            }
            close();
        };

        const bool partial = message.isPartial();
        if (!partial) {
            if (fragment_in_progress_ || payload.size() > kHardMessageLimit) {
                fragment_buffer_.clear();
                fragment_in_progress_ = false;
                rejectOversizedMessage();
                return;
            }
        } else if (message.isFirst()) {
            fragment_buffer_.clear();
            fragment_in_progress_ = true;
            fragment_is_text_ = message.isText();
        } else if (!fragment_in_progress_ || fragment_is_text_ != message.isText()) {
            fragment_buffer_.clear();
            fragment_in_progress_ = false;
            if (callbacks_.on_error) {
                callbacks_.on_error("WebSocket fragment sequence is invalid");
            }
            close();
            return;
        }

        if (partial) {
            if (payload.size() > kHardMessageLimit - fragment_buffer_.size()) {
                fragment_buffer_.clear();
                fragment_in_progress_ = false;
                rejectOversizedMessage();
                return;
            }
            fragment_buffer_.append(payload.data(), payload.size());
            if (!message.isLast()) {
                return;
            }
            fragment_in_progress_ = false;
        }

        const std::string& complete = partial ? fragment_buffer_ : payload;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(complete.data());
        const bool is_text = partial ? fragment_is_text_ : message.isText();
        if (is_text) {
            if (callbacks_.on_text) {
                callbacks_.on_text(data, complete.size());
            }
        } else if (callbacks_.on_binary) {
            callbacks_.on_binary(data, complete.size());
        }
    });
    client_->onEvent([this](websockets::WebsocketsClient&,
                            websockets::WebsocketsEvent event, String data) {
        switch (event) {
            case websockets::WebsocketsEvent::ConnectionOpened:
                connected_ = true;
                break;
            case websockets::WebsocketsEvent::ConnectionClosed:
                connected_ = false;
                fragment_in_progress_ = false;
                // A user may close from inside on_text/on_binary while its data
                // still points into fragment_buffer_. Release only after the
                // outermost ArduinoWebsockets call and callback have unwound.
                fragment_release_pending_ = true;
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

    // A failed handshake may synchronously emit ConnectionClosed. Treat that
    // as part of connect(), not as a second established-session disconnect.
    suppress_close_callback_ = true;
    beginClientCall();
    const bool opened = client_->connect(String(request.url.c_str()));
    endClientCall();
    suppress_close_callback_ = false;
    if (!opened || !connected_) {
        connected_ = false;
        client_.reset();
        if (callbacks_.on_error) {
            callbacks_.on_error("WebSocket TCP/TLS handshake failed");
        }
        return false;
    }
    // ArduinoWebsockets emits ConnectionOpened from inside connect(). Notify
    // the protocol only after connect() has fully returned; sending Xiaozhi's
    // hello from the nested event callback can report success before the
    // underlying WebSocket is ready and the frame is then never delivered.
    if (callbacks_.on_open) {
        callbacks_.on_open();
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
    const bool was_connected = connected_;
    suppress_close_callback_ = true;
    connected_ = false;
    if (client_call_depth_ != 0) {
        close_pending_ = true;
        // Keep the object alive until the outer library call unwinds, but close
        // its socket now so ArduinoWebsockets::poll() stops draining the batch.
        if (was_connected && client_ != nullptr) {
            client_->close();
        }
        return;
    }
    closeNow();
#else
    client_.reset();
#endif
    connected_ = false;
    fragment_buffer_.clear();
    fragment_in_progress_ = false;
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
    std::string().swap(fragment_buffer_);
    fragment_in_progress_ = false;
    fragment_release_pending_ = false;
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
    if (client_call_depth_ == 0 && fragment_release_pending_) {
        std::string().swap(fragment_buffer_);
        fragment_in_progress_ = false;
        fragment_release_pending_ = false;
    }
}

void ArduinoWebSocketTransport::setCACertificate(const char* pem_certificate) {
    ca_certificate_ = pem_certificate;
}

}  // namespace xiaozhi
