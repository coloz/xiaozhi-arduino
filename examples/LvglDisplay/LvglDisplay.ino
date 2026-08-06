/*
 * LvglDisplay 示例
 *
 * 演示使用 LVGL 9 和 TFT_eSPI 显示 Xiaozhi 的连接状态、识别文本、回复及
 * 情绪事件。运行前请安装可选显示/传输依赖，配置 TFT_eSPI、Wi-Fi、服务地址、
 * 令牌和根证书；本示例不包含麦克风、扬声器或 Opus 音频实现。
 *
 * Demonstrates using LVGL 9 and TFT_eSPI to show Xiaozhi connection states,
 * recognized text, replies, and emotion events. Install the optional display and
 * transport dependencies and configure TFT_eSPI, Wi-Fi, the service URL, token,
 * and root CA first. This example does not implement microphone, speaker, or Opus audio.
 */

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>  // Makes the optional transport dependency explicit.
#include <Xiaozhi.h>
#include <lvgl.h>

// Optional dependencies for this example only:
//   LVGL 9.x, TFT_eSPI >= 2.5 and ArduinoWebsockets >= 0.5.4
// Configure TFT_eSPI's controller and pins before flashing. This example uses
// LVGL's v9 display API and an explicit TFT_eSPI flush adapter. The adjacent
// lv_conf.h supplies a self-contained minimal configuration for every LVGL TU.
#if LVGL_VERSION_MAJOR != 9
#error "LvglDisplay requires LVGL 9.x"
#endif

namespace {
constexpr uint16_t kDisplayWidth = 240;
constexpr uint16_t kDisplayHeight = 320;
constexpr uint16_t kBufferRows = 24;
alignas(LV_DRAW_BUF_ALIGN) uint8_t drawBuffer[kDisplayWidth * kBufferRows * 2];

constexpr char kWifiSsid[] = "YOUR_WIFI_SSID";
constexpr char kWifiPassword[] = "YOUR_WIFI_PASSWORD";
constexpr char kWebSocketUrl[] = "wss://your-server.example/xiaozhi/v1/";
constexpr char kToken[] = "YOUR_TOKEN";
constexpr char kRootCa[] = R"PEM(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_YOUR_SERVER_ROOT_CA
-----END CERTIFICATE-----
)PEM";

TFT_eSPI tft(kDisplayWidth, kDisplayHeight);
lv_obj_t* stateLabel = nullptr;
lv_obj_t* roleLabel = nullptr;
lv_obj_t* messageLabel = nullptr;
xiaozhi::ArduinoWebSocketTransport transport;
xiaozhi::Client client(transport);
xiaozhi::ClientRuntime runtime(client);

void flushDisplay(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
  const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  const uint32_t height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, width, height);
  tft.pushColors(reinterpret_cast<uint16_t*>(pixels), width * height, true);
  tft.endWrite();
  lv_display_flush_ready(display);
}

void setMessage(const char* role, const std::string& message) {
  constexpr size_t kMaximumLabelBytes = 256;
  const size_t length =
      message.size() < kMaximumLabelBytes ? message.size() : kMaximumLabelBytes;
  const std::string bounded(message.data(), length);
  lv_label_set_text(roleLabel, role);
  lv_label_set_text(messageLabel, bounded.c_str());
}

void createUi() {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);

  lv_obj_t* title = lv_label_create(screen);
  lv_label_set_text(title, "Xiaozhi Arduino");
  lv_obj_set_style_text_color(title, lv_color_hex(0x48CAE4), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  stateLabel = lv_label_create(screen);
  lv_label_set_text(stateLabel, "starting");
  lv_obj_set_style_text_color(stateLabel, lv_color_hex(0xFFD166), 0);
  lv_obj_align(stateLabel, LV_ALIGN_TOP_LEFT, 12, 48);

  roleLabel = lv_label_create(screen);
  lv_label_set_text(roleLabel, "system");
  lv_obj_set_style_text_color(roleLabel, lv_color_hex(0x80ED99), 0);
  lv_obj_align(roleLabel, LV_ALIGN_TOP_LEFT, 12, 82);

  messageLabel = lv_label_create(screen);
  lv_label_set_text(messageLabel, "Waiting for Xiaozhi");
  lv_label_set_long_mode(messageLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(messageLabel, kDisplayWidth - 24);
  lv_obj_set_style_text_color(messageLabel, lv_color_hex(0xF1FAEE), 0);
  lv_obj_align(messageLabel, LV_ALIGN_TOP_LEFT, 12, 108);
}

void onEvent(const xiaozhi::Event& event) {
  switch (event.type) {
    case xiaozhi::EventType::Stt:
      setMessage("user", event.text);
      break;
    case xiaozhi::EventType::TtsSentence:
      setMessage("assistant", event.text);
      break;
    case xiaozhi::EventType::Emotion:
      setMessage("emotion", event.emotion);
      break;
    case xiaozhi::EventType::Alert:
      setMessage(event.status.c_str(), event.text);
      break;
    default:
      break;
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(0);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t* display = lv_display_create(kDisplayWidth, kDisplayHeight);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display, flushDisplay);
  lv_display_set_buffers(display, drawBuffer, nullptr, sizeof(drawBuffer),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  createUi();

  WiFi.begin(kWifiSsid, kWifiPassword);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    lv_label_set_text(stateLabel, "wifi failed");
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
    lv_label_set_text(stateLabel, xiaozhi::stateName(next));
  };
  callbacks.on_event = onEvent;
  callbacks.on_error = [](xiaozhi::ErrorCode, const std::string& message) {
    lv_label_set_text(stateLabel, "error");
    setMessage("system", message);
  };

  if (runtime.begin(config, callbacks)) {
    runtime.requestStartListening(xiaozhi::ListeningMode::AutoStop);
  }
}

void loop() {
  runtime.loop();
  lv_timer_handler();
  delay(2);
}
