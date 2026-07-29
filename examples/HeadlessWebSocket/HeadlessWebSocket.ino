/*
 * HeadlessWebSocket 示例
 *
 * 演示 ESP32 在不接显示屏和音频驱动的情况下，通过安全 WebSocket 连接
 * Xiaozhi 服务，并在串口输出状态、事件和下行音频信息。运行前请填写 Wi-Fi、
 * WebSocket 地址、令牌及根证书；如需播放语音，还要在 on_audio 中接入解码器。
 *
 * Demonstrates an ESP32 connecting to a Xiaozhi service over secure WebSocket
 * without a display or audio driver, while logging states, events, and downlink
 * audio information to Serial. Before use, set the Wi-Fi, WebSocket URL, token,
 * and root CA; add a decoder in on_audio if speech playback is required.
 */

#include <WiFi.h>
#include <ArduinoWebsockets.h>  // Makes the optional transport dependency explicit.
#include <Xiaozhi.h>

namespace {
constexpr char kWifiSsid[] = "YOUR_WIFI_SSID";
constexpr char kWifiPassword[] = "YOUR_WIFI_PASSWORD";
constexpr char kWebSocketUrl[] = "wss://your-server.example/xiaozhi/v1/";
constexpr char kToken[] = "YOUR_TOKEN";

// Paste the PEM root CA for your server. The example deliberately refuses insecure WSS.
constexpr char kRootCa[] = R"PEM(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_YOUR_SERVER_ROOT_CA
-----END CERTIFICATE-----
)PEM";

xiaozhi::ArduinoWebSocketTransport transport;
xiaozhi::Client client(transport);
}  // namespace

void setup() {
  Serial.begin(115200);
  WiFi.begin(kWifiSsid, kWifiPassword);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  transport.setCACertificate(kRootCa);

  xiaozhi::ClientConfig config;
  config.websocket_url = kWebSocketUrl;
  config.authorization = kToken;
  config.device_id = xiaozhi::Esp32Identity::deviceId();
  config.client_id = xiaozhi::Esp32Identity::persistentClientId();

  xiaozhi::Callbacks callbacks;
  callbacks.on_state_changed = [](xiaozhi::State, xiaozhi::State next) {
    Serial.printf("state: %s\n", xiaozhi::stateName(next));
  };
  callbacks.on_event = [](const xiaozhi::Event& event) {
    Serial.printf("event=%u text=%s emotion=%s\n", static_cast<unsigned>(event.type),
                  event.text.c_str(), event.emotion.c_str());
  };
  callbacks.on_audio = [](const xiaozhi::AudioFrame& frame) {
    // Feed frame.opus to an external Opus decoder/audio driver here.
    Serial.printf("audio: %u bytes @ %lu Hz\n", static_cast<unsigned>(frame.opus.size()),
                  static_cast<unsigned long>(frame.format.sample_rate));
  };
  callbacks.on_error = [](xiaozhi::ErrorCode code, const std::string& message) {
    Serial.printf("error[%s]: %s\n", xiaozhi::errorName(code), message.c_str());
  };

  if (!client.begin(config, callbacks)) {
    Serial.println("Xiaozhi configuration failed");
    return;
  }
  client.startListening(xiaozhi::ListeningMode::AutoStop);
}

void loop() {
  client.loop();
  delay(1);
}
