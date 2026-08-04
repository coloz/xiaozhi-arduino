/*
 * U8g2RobotEyesEmotion example
 *
 * Receives Xiaozhi LLM emotion events, prints their typed state to Serial,
 * and forwards the exact protocol string to U8g2RobotEyes.
 *
 * Required libraries:
 *   - U8g2RobotEyes >= 1.3.0
 *   - U8g2 >= 2.36
 *   - ArduinoWebsockets >= 0.5.4
 *
 * Before uploading, copy Secrets.example.h to Secrets.h and set Wi-Fi there.
 * Without that file, the sketch cycles all 21 Xiaozhi expressions offline.
 * This display-only example does not provide microphone or speaker audio I/O.
 */

#include <ArduinoWebsockets.h>
#include <U8g2lib.h>

// Select the library's pixel-exact Xiaozhi 128x64 bitmap theme before include.
#define U8G2_ROBOT_EYES_STYLE U8G2_ROBOT_EYES_STYLE_XIAOZHI
#include <U8g2RobotEyes.h>

#include <WiFi.h>
#include <Xiaozhi.h>

#include <cstring>

#if __has_include("Secrets.h")
#include "Secrets.h"
#else
#define XIAOZHI_WIFI_SSID "YOUR_WIFI_SSID"
#define XIAOZHI_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
U8g2RobotEyes eyes(display);
xiaozhi::ArduinoWebSocketTransport transport;

namespace {

constexpr char kWifiSsid[] = XIAOZHI_WIFI_SSID;
constexpr char kWifiPassword[] = XIAOZHI_WIFI_PASSWORD;
constexpr uint16_t kExpressionTransitionMs = 360;

std::string pendingEmotionName = "neutral";
std::string currentEmotionName = "neutral";
xiaozhi::Emotion currentEmotion = xiaozhi::Emotion::Neutral;
bool expressionPending = false;
bool demoMode = false;
uint8_t demoEmotionCode = static_cast<uint8_t>(xiaozhi::Emotion::Neutral);
uint32_t nextDemoEmotionMs = 0;
uint32_t lastHeartbeatMs = 0;

void queueEmotion(xiaozhi::Emotion emotion, const std::string& rawName) {
  U8g2RobotEyes::XiaozhiExpression displayExpression;
  if (!U8g2RobotEyes::expressionFromName(rawName.c_str(), displayExpression)) {
    Serial.printf("[display] unsupported Xiaozhi emotion: %s\n",
                  rawName.c_str());
    return;
  }

  pendingEmotionName = rawName;
  currentEmotion = emotion;
  expressionPending = true;

  // Replace Serial with Serial1 if a second MCU consumes this status line.
  Serial.printf("EMOTION:%u,%s DISPLAY:%u,%s\n",
                static_cast<unsigned>(emotion), rawName.c_str(),
                static_cast<unsigned>(displayExpression),
                U8g2RobotEyes::expressionName(displayExpression));
}

void onEvent(const xiaozhi::Event& event) {
  switch (event.type) {
    case xiaozhi::EventType::Emotion:
    case xiaozhi::EventType::Alert:
      queueEmotion(event.emotion_type, event.emotion);
      break;
    default:
      break;
  }
}

void queueLocalEmotion(const char* name) {
  queueEmotion(xiaozhi::emotionFromName(name), name);
}

bool hasPlaceholderNetworkConfig() {
  return std::strcmp(kWifiSsid, "YOUR_WIFI_SSID") == 0;
}

void updateDemo(uint32_t now) {
  if (!demoMode || static_cast<int32_t>(now - nextDemoEmotionMs) < 0) {
    return;
  }

  const xiaozhi::Emotion emotion =
      static_cast<xiaozhi::Emotion>(demoEmotionCode);
  queueEmotion(emotion, xiaozhi::emotionName(emotion));
  ++demoEmotionCode;
  if (demoEmotionCode > static_cast<uint8_t>(xiaozhi::Emotion::Confused)) {
    demoEmotionCode = static_cast<uint8_t>(xiaozhi::Emotion::Neutral);
  }
  nextDemoEmotionMs = now + 2200;
}

}  // namespace

// Keep Client after callback-owned state so its destructor runs first.
xiaozhi::Client client(transport);

void setup() {
  Serial.begin(115200);

  display.begin();
  eyes.begin(128, 64, 20);
  eyes.setAutoBlink(true, 3200, 1200);
  eyes.setIdle(true, 2200);
  eyes.setExpression("neutral", 0);

  if (hasPlaceholderNetworkConfig()) {
    demoMode = true;
    nextDemoEmotionMs = millis() + 1000;
    Serial.println("[demo] network placeholders detected; cycling Xiaozhi emotions");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPassword);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    queueLocalEmotion("shocked");
    Serial.println("[wifi] connection failed");
    return;
  }

  xiaozhi::ClientConfig config;
  xiaozhi::ProvisioningResult provisioning;
  std::string provisioningError;
  xiaozhi::OfficialServiceOptions serviceOptions;
  serviceOptions.board_name = "ESP32-S3 OLED U8g2RobotEyes";
  Serial.println("[network] fetching official Xiaozhi configuration");
  if (!xiaozhi::ArduinoOfficialService::configure(
          config, provisioning, provisioningError, serviceOptions)) {
    queueLocalEmotion("shocked");
    Serial.printf("[xiaozhi] provisioning failed: %s\n",
                  provisioningError.c_str());
    return;
  }
  if (provisioning.activation.present && !provisioning.activation.code.empty()) {
    Serial.printf("[xiaozhi] activation code=%s message=%s\n",
                  provisioning.activation.code.c_str(),
                  provisioning.activation.message.c_str());
  }
  transport.setCACertificate(
      xiaozhi::ArduinoOfficialService::rootCACertificate());

  xiaozhi::Callbacks callbacks;
  callbacks.on_event = onEvent;
  callbacks.on_state_changed = [](xiaozhi::State, xiaozhi::State next) {
    Serial.printf("[xiaozhi] state=%s\n", xiaozhi::stateName(next));
  };
  callbacks.on_error = [](xiaozhi::ErrorCode code, const std::string& message) {
    queueLocalEmotion("shocked");
    Serial.printf("[xiaozhi] error[%s]: %s\n", xiaozhi::errorName(code),
                  message.c_str());
  };

  if (!client.begin(config, callbacks)) {
    queueLocalEmotion("shocked");
    return;
  }

  // Starts a session so the server can send LLM emotion events.
  client.startListening(xiaozhi::ListeningMode::AutoStop);
}

void loop() {
  client.loop();
  updateDemo(millis());

  // Apply display work after Client::loop() has returned from callbacks.
  if (expressionPending) {
    expressionPending = false;
    currentEmotionName = pendingEmotionName;
    eyes.setExpression(pendingEmotionName.c_str(), kExpressionTransitionMs);
  }
  eyes.update();

  const uint32_t now = millis();
  if (now - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = now;
    Serial.printf(
        "[heartbeat] wifi=%d client=%s emotion=%u,%s display=%u,%s heap=%lu\n",
        static_cast<int>(WiFi.status()), client.stateName(),
        static_cast<unsigned>(currentEmotion),
        xiaozhi::emotionName(currentEmotion),
        static_cast<unsigned>(eyes.xiaozhiExpression()),
        currentEmotionName.c_str(),
        static_cast<unsigned long>(ESP.getFreeHeap()));
  }

  delay(1);
}
