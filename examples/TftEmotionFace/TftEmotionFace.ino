/*
 * TftEmotionFace example
 *
 * Complete Xiaozhi voice terminal with TftRobotEyes expressions. It receives
 * Xiaozhi LLM emotion events, prints their typed state to Serial, and forwards
 * the exact protocol string to TftRobotEyes. A compile-time selected audio
 * preset handles microphone capture, Opus playback, and WakeNet. This sketch
 * owns its TFT and BOOT-button configuration.
 *
 * Required libraries:
 *   - TftRobotEyes >= 1.1.0
 *   - TFT_eSPI >= 2.5 (configure User_Setup for your display first)
 *   - ArduinoWebsockets >= 0.5.4
 *   - EspressifEs8311 (only for the ES8311 profile)
 *   - EspressifOpus
 *
 * Before uploading, copy Secrets.example.h to Secrets.h and set Wi-Fi there.
 * The official Xiaozhi service supplies the WebSocket URL and token securely.
 * Select a 16 MB ESP-SR partition and flash a model image containing
 * wn9_nihaoxiaozhi_tts, otherwise the phrase "你好小智" cannot be detected.
 */

// Change this one line to XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX for AI VOX audio.
#ifndef XIAOZHI_AUDIO_BOARD
#define XIAOZHI_AUDIO_BOARD XIAOZHI_AUDIO_BOARD_OJ_ESP32S3_BASIC
#endif

#include <Arduino.h>
#include <Xiaozhi.h>

#include <ArduinoWebsockets.h>
#if XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR
#include <ESP_SR.h>
#endif
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
#include <EspressifEs8311.h>
#endif
#include <EspressifOpus.h>
#include <TFT_eSPI.h>
#include <TftRobotEyes.h>
#include <WiFi.h>

#include <cstring>

#ifndef XIAOZHI_DEVICE_BOARD_TYPE
#define XIAOZHI_DEVICE_BOARD_TYPE "esp32s3-tft-emotion-face"
#endif
#ifndef XIAOZHI_DEVICE_BOARD_NAME
#define XIAOZHI_DEVICE_BOARD_NAME "ESP32-S3 TFT Emotion Face"
#endif
#ifndef XIAOZHI_CHAT_BUTTON_PIN
#define XIAOZHI_CHAT_BUTTON_PIN 0
#endif
#ifndef XIAOZHI_CHAT_BUTTON_ACTIVE_LEVEL
#define XIAOZHI_CHAT_BUTTON_ACTIVE_LEVEL LOW
#endif
#ifndef XIAOZHI_TFT_ROTATION
#define XIAOZHI_TFT_ROTATION 0
#endif

#ifndef XIAOZHI_ENABLE_SERVER_AEC_DEFAULT
#define XIAOZHI_ENABLE_SERVER_AEC_DEFAULT 1
#endif
#ifndef XIAOZHI_ENABLE_VOICE_BARGE_IN_DEFAULT
#define XIAOZHI_ENABLE_VOICE_BARGE_IN_DEFAULT 1
#endif
#if XIAOZHI_ENABLE_VOICE_BARGE_IN_DEFAULT && \
    !XIAOZHI_ENABLE_SERVER_AEC_DEFAULT
#error "Voice barge-in requires server AEC in this audio pipeline"
#endif
#ifndef XIAOZHI_PROTOCOL_VERSION_OVERRIDE
#define XIAOZHI_PROTOCOL_VERSION_OVERRIDE 0
#endif
#if XIAOZHI_PROTOCOL_VERSION_OVERRIDE < 0 || \
    XIAOZHI_PROTOCOL_VERSION_OVERRIDE > 3
#error "XIAOZHI_PROTOCOL_VERSION_OVERRIDE must be 0, 1, 2, or 3"
#endif
#ifndef XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC
#define XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC 0
#endif
#if XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC != 0 && \
    XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC != 1
#error "XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC must be 0 or 1"
#endif

// Opus encoding/decoding runs on the audio codec task; keep enough room for
// JSON and UI callbacks without reserving the former 48 KiB loop stack.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#if __has_include("Secrets.h")
#include "Secrets.h"
#else
#define XIAOZHI_WIFI_SSID "YOUR_WIFI_SSID"
#define XIAOZHI_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

TFT_eSPI tft;
TftRobotEyes eyes(tft, TftRobotEyes::PLAIN_BASIC);
xiaozhi::ArduinoWebSocketTransport transport;

I2sOpusAudioPort::Config makeAudioConfig() {
  I2sOpusAudioPort::Config config = xiaozhi_audio_board::makeConfig();
  config.captureLogIntervalPackets = 10;
  config.enableSpeechConditioning = true;
  config.speechGateRms = 300;
  config.speechTargetRms = 2200;
  config.speechMaximumGain = 4.0f;
  config.speechSilenceGain = 0.25f;
  config.speechStartPackets = 2;
  config.speechHoldMs = 420;
  config.wakeModelKeyword = "nihaoxiaozhi";
  config.defaultWakeWord = "你好小智";
  return config;
}

const I2sOpusAudioPort::Config audioConfig = makeAudioConfig();
I2sOpusAudioPort audioPort(audioConfig);

namespace {

constexpr char kWifiSsid[] = XIAOZHI_WIFI_SSID;
constexpr char kWifiPassword[] = XIAOZHI_WIFI_PASSWORD;

constexpr uint16_t kExpressionTransitionMs = 600;
constexpr uint32_t kChatButtonDebounceMs = 40;
constexpr uint32_t kSpeakingSilenceTimeoutMs = 12000;

std::string pendingEmotionName = "neutral";
std::string currentEmotionName = "neutral";
xiaozhi::Emotion currentEmotion = xiaozhi::Emotion::Neutral;
bool expressionPending = false;
bool eyesReady = false;
bool demoMode = false;
bool activationRequired = false;
bool fatalScreen = false;
bool clientStarted = false;
bool audioHardwareReady = false;
uint8_t demoEmotionCode = static_cast<uint8_t>(xiaozhi::Emotion::Neutral);
uint32_t nextDemoEmotionMs = 0;
uint32_t activationRefreshAtMs = 0;
uint32_t lastActivationCountdownSec = 0xFFFFFFFFUL;
uint32_t lastHeartbeatMs = 0;
uint32_t lastSpeakingAudioMs = 0;
bool chatButtonReading = HIGH;
bool chatButtonStableState = HIGH;
uint32_t chatButtonChangedMs = 0;

void queueEmotion(xiaozhi::Emotion emotion, const std::string& rawName) {
  TftRobotEyes::XiaozhiExpression displayExpression;
  if (!TftRobotEyes::expressionFromName(rawName.c_str(), displayExpression)) {
    Serial.printf("[display] unsupported Xiaozhi emotion: %s\n",
                  rawName.c_str());
    return;
  }

  pendingEmotionName = rawName;
  currentEmotion = emotion;
  expressionPending = true;

  // Numeric Emotion values are stable and suitable for a local UART protocol.
  // Replace Serial with Serial1 here if another MCU consumes this line.
  Serial.printf("EMOTION:%u,%s DISPLAY:%u,%s\n",
                static_cast<unsigned>(emotion), rawName.c_str(),
                static_cast<unsigned>(displayExpression),
                TftRobotEyes::expressionName(displayExpression));
}

void receiveEmotion(const xiaozhi::Event& event) {
  queueEmotion(event.emotion_type, event.emotion);
}

void queueLocalEmotion(const char* name);

void onEvent(const xiaozhi::Event& event) {
  switch (event.type) {
    case xiaozhi::EventType::Stt:
      Serial.printf("[xiaozhi] STT: %s\n", event.text.c_str());
      break;
    case xiaozhi::EventType::TtsSentence:
      Serial.printf("[xiaozhi] TTS: %s\n", event.text.c_str());
      break;
    case xiaozhi::EventType::Emotion:
      receiveEmotion(event);
      break;
    case xiaozhi::EventType::Alert:
      queueLocalEmotion("shocked");
      Serial.printf("[xiaozhi] alert[%s]: %s\n", event.status.c_str(),
                    event.text.c_str());
      break;
    default:
      break;
  }
}

void queueLocalEmotion(const char* name) {
  queueEmotion(xiaozhi::emotionFromName(name), name);
}

void showSystemScreen(const char* title, const char* line1,
                      const char* line2 = nullptr) {
  fatalScreen = true;
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(false, false);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString(title, tft.width() / 2, 24, 1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString(line1, tft.width() / 2, 76, 1);
  if (line2 != nullptr) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(line2, tft.width() / 2, 98, 1);
  }
  tft.setTextDatum(TL_DATUM);
}

void drawActivationCountdown(uint32_t seconds) {
  if (seconds == lastActivationCountdownSec) {
    return;
  }
  lastActivationCountdownSec = seconds;

  char text[48];
  snprintf(text, sizeof(text), "Checking registration in %lu s",
           static_cast<unsigned long>(seconds));
  const int16_t footerY = tft.height() - 20;
  tft.fillRect(0, footerY, tft.width(), 20, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString(text, tft.width() / 2, footerY + 5, 1);
  tft.setTextDatum(TL_DATUM);
}

void showActivationScreen(const std::string& code) {
  const int16_t centerX = tft.width() / 2;
  const bool compact = tft.height() < 200;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextWrap(false, false);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("XIAOZHI SETUP", centerX, compact ? 4 : 10, 1);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("NOT REGISTERED", centerX, compact ? 26 : 42, 1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(compact ? 2 : 3);
  tft.drawString("xiaozhi.me", centerX, compact ? 50 : 78, 1);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("ENTER ACTIVATION CODE", centerX, compact ? 76 : 118, 1);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(compact ? 3 : 4);
  tft.drawString(code.c_str(), centerX, compact ? 90 : 140, 1);
  tft.setTextDatum(TL_DATUM);

  lastActivationCountdownSec = 0xFFFFFFFFUL;
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

// Keep Runtime after Client and callback-owned state so it stops first.
xiaozhi::Client client(transport);
xiaozhi::ClientRuntime runtime(client);

void toggleDialogue(const char* source) {
  if (!runtime.ready()) {
    Serial.printf("[xiaozhi] %s ignored: client is not ready\n", source);
    return;
  }
  if (runtime.state() == xiaozhi::State::Connecting) {
    Serial.printf("[xiaozhi] %s ignored: already connecting\n", source);
    return;
  }
  Serial.printf("[xiaozhi] dialogue toggle from %s\n", source);
  if (!runtime.requestToggleChat()) {
    Serial.printf("[xiaozhi] dialogue toggle failed in state=%s\n",
                  runtime.stateName());
  }
}

void pollDialogueTriggers() {
  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    if (command == 't' || command == 'T') {
      toggleDialogue("serial");
    }
  }

  const uint32_t now = millis();
  const bool reading = digitalRead(XIAOZHI_CHAT_BUTTON_PIN);
  if (reading != chatButtonReading) {
    chatButtonReading = reading;
    chatButtonChangedMs = now;
  }
  if (reading != chatButtonStableState &&
      now - chatButtonChangedMs >= kChatButtonDebounceMs) {
    chatButtonStableState = reading;
    if (chatButtonStableState == XIAOZHI_CHAT_BUTTON_ACTIVE_LEVEL) {
      toggleDialogue("BOOT button");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);
  pinMode(XIAOZHI_CHAT_BUTTON_PIN, INPUT_PULLUP);
  Serial.printf("[audio] profile=%s\n",
                I2sOpusAudioPort::Config::compiledProfileName());

  tft.init();
  tft.setRotation(XIAOZHI_TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);

  eyesReady = eyes.begin(50);
  if (!eyesReady) {
    tft.fillScreen(TFT_RED);
    Serial.println("[display] TftRobotEyes sprite allocation failed");
    return;
  }
  eyes.setAutoBlink(true, 3000, 1600);
  eyes.setIdle(true, 1700);
  eyes.setExpression("neutral", 0);

  const auto& audioOutput = audioConfig.hardware.output;
  Serial.printf("[audio] profile=%s OUT port=%d MCLK=%d BCLK=%d WS=%d DATA=%d\n",
                I2sOpusAudioPort::Config::compiledProfileName(),
                audioOutput.port, audioOutput.mclk, audioOutput.bclk,
                audioOutput.ws, audioOutput.data);
  audioHardwareReady = xiaozhi_audio_board::probe(audioConfig);
  if (!audioHardwareReady) {
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
    Serial.printf("[audio] ES8311 not detected at I2C address 0x%02X\n",
                  audioConfig.hardware.es8311.address);
#else
    Serial.println("[audio] selected audio hardware probe failed");
#endif
  } else {
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
    Serial.printf("[audio] ES8311 detected at I2C address 0x%02X\n",
                  audioConfig.hardware.es8311.address);
#else
    Serial.println("[audio] direct audio pins configured; runtime probe unavailable");
#endif
  }

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
  serviceOptions.board_type = XIAOZHI_DEVICE_BOARD_TYPE;
  serviceOptions.board_name = XIAOZHI_DEVICE_BOARD_NAME;
  Serial.println("[network] fetching official Xiaozhi configuration");
  if (!xiaozhi::ArduinoOfficialService::configure(
          config, provisioning, provisioningError, serviceOptions)) {
    queueLocalEmotion("shocked");
    Serial.printf("[xiaozhi] provisioning failed: %s\n",
                  provisioningError.c_str());
    return;
  }
#if XIAOZHI_PROTOCOL_VERSION_OVERRIDE != 0
  config.protocol_version = XIAOZHI_PROTOCOL_VERSION_OVERRIDE;
#endif
  const bool aecRequested = XIAOZHI_ENABLE_SERVER_AEC_DEFAULT != 0;
  const bool timedServerAec = config.protocol_version == 2;
  const bool allowUntimestampedAec =
      XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC != 0;
  config.enable_server_aec =
      aecRequested && (timedServerAec || allowUntimestampedAec);
  config.enable_voice_barge_in =
      XIAOZHI_ENABLE_VOICE_BARGE_IN_DEFAULT != 0 &&
      config.enable_server_aec;
  Serial.printf("[xiaozhi] official WebSocket protocol v%u, token present=%s\n",
                config.protocol_version,
                config.authorization.empty() ? "no" : "yes");
  Serial.printf("[audio] server AEC=%s voice barge-in=%s\n",
                config.enable_server_aec ? "on" : "off",
                config.enable_voice_barge_in ? "on" : "off");
  if (aecRequested && !config.enable_server_aec) {
    Serial.println("[audio] protocol has no AEC timestamp; using AutoStop to prevent echo self-interruption");
  }
  if (provisioning.activation.present && !provisioning.activation.code.empty()) {
    Serial.printf("[xiaozhi] activation code=%s message=%s\n",
                  provisioning.activation.code.c_str(),
                  provisioning.activation.message.c_str());
    activationRequired = true;
    const uint32_t refreshDelayMs =
        constrain(provisioning.activation.timeout_ms, 10000UL, 60000UL);
    activationRefreshAtMs = millis() + refreshDelayMs;
    showActivationScreen(provisioning.activation.code);
    Serial.printf("[xiaozhi] registration guide displayed; refresh in %lu ms\n",
                  static_cast<unsigned long>(refreshDelayMs));
    return;
  }
  if (!audioHardwareReady) {
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
    showSystemScreen("AUDIO ERROR", "ES8311 NOT FOUND",
                     "CHECK SDA41 SCL42");
#else
    showSystemScreen("AUDIO ERROR", "AUDIO NOT READY",
                     "CHECK AUDIO PINS");
#endif
    return;
  }
  transport.setCACertificate(
      xiaozhi::ArduinoOfficialService::rootCACertificate());

  xiaozhi::Callbacks callbacks;
  callbacks.on_event = onEvent;
  callbacks.on_wake_word = [](const std::string& wakeWord) {
    Serial.printf("[wake] detected: %s\n", wakeWord.c_str());
  };
  callbacks.on_state_changed = [](xiaozhi::State, xiaozhi::State next) {
    Serial.printf("[xiaozhi] state=%s\n", xiaozhi::stateName(next));
    const bool lowLatencySession = next == xiaozhi::State::Connecting ||
                                   next == xiaozhi::State::Listening ||
                                   next == xiaozhi::State::Speaking;
    WiFi.setSleep(!lowLatencySession);
    audioPort.setWakeDetectionEnabled(next == xiaozhi::State::Idle);
    if (next == xiaozhi::State::Listening) {
      lastSpeakingAudioMs = 0;
      queueLocalEmotion("thinking");
    } else if (next == xiaozhi::State::Speaking) {
      lastSpeakingAudioMs = millis();
    } else if (next == xiaozhi::State::Idle) {
      lastSpeakingAudioMs = 0;
      queueLocalEmotion("neutral");
    }
  };
  callbacks.on_audio = [](const xiaozhi::AudioFrame&) {
    lastSpeakingAudioMs = millis();
  };
  callbacks.on_capture = [](bool enabled, const xiaozhi::AudioFormat& format) {
    Serial.printf("[xiaozhi] capture=%s format=%lu Hz/%u ch/%u ms\n",
                  enabled ? "on" : "off",
                  static_cast<unsigned long>(format.sample_rate),
                  format.channels, format.frame_duration_ms);
  };
  callbacks.on_error = [](xiaozhi::ErrorCode code, const std::string& message) {
    // The official service may close an otherwise completed session after
    // returning to idle. Keep that normal close from replacing the last
    // server-provided expression with an error face.
    if (code != xiaozhi::ErrorCode::TransportDisconnected) {
      queueLocalEmotion("shocked");
    }
    Serial.printf("[xiaozhi] error[%s]: %s\n", xiaozhi::errorName(code),
                  message.c_str());
  };

  if (!client.attachAudioPort(&audioPort)) {
    showSystemScreen("AUDIO ERROR", "AUDIO PORT FAILED");
    Serial.println("[audio] failed to attach I2S/Opus audio port");
    return;
  }
  if (!runtime.begin(config, callbacks)) {
    showSystemScreen("STARTUP ERROR", "AUDIO OR CLIENT FAILED",
                     "CHECK SERIAL LOG");
    return;
  }
  clientStarted = true;
  Serial.println(
      "[xiaozhi] ready; say 你好小智, press BOOT, or send 't'");
}

void loop() {
  if (activationRequired) {
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - activationRefreshAtMs) >= 0) {
      Serial.println("[xiaozhi] refreshing activation status");
      Serial.flush();
      delay(50);
      ESP.restart();
    }
    const uint32_t remainingMs = activationRefreshAtMs - now;
    drawActivationCountdown((remainingMs + 999UL) / 1000UL);
    delay(10);
    return;
  }

  if (fatalScreen) {
    delay(20);
    return;
  }

  if (clientStarted) {
    runtime.loop();
    pollDialogueTriggers();
  }
  updateDemo(millis());

  // Apply UI work after Runtime::loop() has dispatched user callbacks.
  if (eyesReady && expressionPending) {
    expressionPending = false;
    currentEmotionName = pendingEmotionName;
    eyes.setExpression(pendingEmotionName.c_str(), kExpressionTransitionMs);
  }
  if (eyesReady) {
    eyes.update();
  }

  const uint32_t now = millis();
  if (clientStarted && runtime.ready() &&
      runtime.state() == xiaozhi::State::Speaking &&
      lastSpeakingAudioMs != 0 &&
      now - lastSpeakingAudioMs >= kSpeakingSilenceTimeoutMs &&
      audioPort.playbackIdle()) {
    Serial.println(
        "[xiaozhi] speaking watchdog: closing stale session");
    lastSpeakingAudioMs = 0;
    runtime.requestCloseSession();
  }
  if (now - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = now;
    Serial.printf(
        "[heartbeat] eyes=%s wifi=%d client=%s emotion=%u,%s "
        "display=%u,%s heap=%lu\n",
        eyesReady ? "ready" : "failed", static_cast<int>(WiFi.status()),
        runtime.stateName(), static_cast<unsigned>(currentEmotion),
        xiaozhi::emotionName(currentEmotion),
        static_cast<unsigned>(eyes.xiaozhiExpression()),
        currentEmotionName.c_str(),
        static_cast<unsigned long>(ESP.getFreeHeap()));
  }

  delay(1);
}
