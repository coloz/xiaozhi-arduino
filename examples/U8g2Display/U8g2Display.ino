/*
 * U8g2Display 示例
 *
 * 演示使用 U8g2 单色 OLED 显示 Xiaozhi 的连接状态、识别文本、回复及情绪事件。
 * 运行前请安装 U8g2 和 ArduinoWebsockets，按硬件调整 OLED 构造器与引脚，并
 * 填写 Wi-Fi、WebSocket 地址、令牌和根证书；本示例不包含音频输入输出。
 *
 * Demonstrates using a monochrome U8g2 OLED to show Xiaozhi connection states,
 * recognized text, replies, and emotion events. Install U8g2 and ArduinoWebsockets,
 * adjust the OLED constructor and pins for the hardware, and set the Wi-Fi,
 * WebSocket URL, token, and root CA. This example does not provide audio I/O.
 */

#include <U8g2lib.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>  // Makes the optional transport dependency explicit.
#include <Xiaozhi.h>

// Optional dependencies for this example only:
//   U8g2 >= 2.36 and ArduinoWebsockets >= 0.5.4
// Change the constructor and Wire pins to match your OLED.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
xiaozhi::ArduinoWebSocketTransport transport;

namespace {
constexpr char kWifiSsid[] = "YOUR_WIFI_SSID";
constexpr char kWifiPassword[] = "YOUR_WIFI_PASSWORD";
constexpr char kWebSocketUrl[] = "wss://your-server.example/xiaozhi/v1/";
constexpr char kToken[] = "YOUR_TOKEN";
constexpr char kRootCa[] = R"PEM(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_YOUR_SERVER_ROOT_CA
-----END CERTIFICATE-----
)PEM";

std::string statusLine = "starting";
std::string messageLine = "Xiaozhi Arduino";
bool displayDirty = true;
uint32_t lastDrawMs = 0;

void setMessage(const std::string& message) {
  const size_t length = message.size() < 256 ? message.size() : 256;
  messageLine.assign(message.data(), length);
  displayDirty = true;
}

void redraw() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 11, "Xiaozhi");
  oled.drawHLine(0, 14, 128);
  oled.drawUTF8(0, 30, statusLine.c_str());
  oled.drawUTF8(0, 48, messageLine.c_str());
  oled.sendBuffer();
}

void onEvent(const xiaozhi::Event& event) {
  switch (event.type) {
    case xiaozhi::EventType::Stt:
      setMessage(std::string("> ") + event.text);
      break;
    case xiaozhi::EventType::TtsSentence:
      setMessage(std::string("< ") + event.text);
      break;
    case xiaozhi::EventType::Emotion:
      setMessage(std::string("emotion: ") + event.emotion);
      break;
    case xiaozhi::EventType::Alert:
      setMessage(event.text);
      break;
    default:
      return;
  }
  displayDirty = true;
}
}  // namespace

// Keep Runtime after Client and callback-owned strings so it stops first.
xiaozhi::Client client(transport);
xiaozhi::ClientRuntime runtime(client);

void setup() {
  Serial.begin(115200);
  oled.begin();
  redraw();

  WiFi.begin(kWifiSsid, kWifiPassword);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    statusLine = "wifi failed";
    displayDirty = true;
    return;
  }

  transport.setCACertificate(kRootCa);
  xiaozhi::ClientConfig config;
  config.websocket_url = kWebSocketUrl;
  config.authorization = kToken;
  config.device_id = xiaozhi::Esp32Identity::deviceId();
  config.client_id = xiaozhi::Esp32Identity::persistentClientId();

  xiaozhi::Callbacks callbacks;
  callbacks.on_state_changed = [](xiaozhi::State, xiaozhi::State next) {
    statusLine = xiaozhi::stateName(next);
    displayDirty = true;
  };
  callbacks.on_event = onEvent;
  callbacks.on_error = [](xiaozhi::ErrorCode, const std::string& message) {
    statusLine = "error";
    setMessage(message);
  };

  if (runtime.begin(config, callbacks)) {
    runtime.requestStartListening(xiaozhi::ListeningMode::AutoStop);
  }
}

void loop() {
  runtime.loop();
  const uint32_t now = millis();
  if (displayDirty && now - lastDrawMs >= 80) {
    displayDirty = false;
    lastDrawMs = now;
    redraw();
  }
  delay(1);
}
