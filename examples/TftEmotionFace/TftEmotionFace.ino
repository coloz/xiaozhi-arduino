/*
 * TftEmotionFace example
 *
 * Complete Xiaozhi voice terminal with grokEyes expressions. It receives
 * Xiaozhi LLM emotion events, prints their typed state to Serial, and forwards
 * the exact protocol string to grokEyes. A compile-time selected audio
 * preset handles microphone capture, Opus playback, and WakeNet. This sketch
 * owns its TFT and BOOT-button configuration.
 *
 * Required libraries:
 *   - grokEyes
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

// Change this one line to NULLLAB_AI_VOX for first-generation AI VOX audio.
// For the ES8311-based AI-VOX3, use NULLLAB_AI_VOX3.
// OJoy: select OJ_ESP32S3_OJOY and use build_ojoy.ps1 for CH13613 QSPI.
#ifndef XIAOZHI_BOARD
#define XIAOZHI_BOARD OJ_ESP32S3_BASIC
#endif

#include <Arduino.h>
#include <Xiaozhi.h>

#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include <ArduinoWebsockets.h>
#if XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR
#include <ESP_SR.h>
#endif
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
#include <EspressifEs8311.h>
#endif
#include <EspressifOpus.h>
#include <TFT_eSPI.h>
#include <grokEyes.h>
#include <WiFi.h>

#if XIAOZHI_BOARD == OJ_ESP32S3_OJOY
#if !defined(CH13613_DRIVER) || defined(ST7789_DRIVER) || \
    TFT_WIDTH != 480 || TFT_HEIGHT != 480
#error "OJoy requires the CH13613 480x480 setup; use build_ojoy.ps1"
#endif
#endif

#include <cstring>

#ifndef XIAOZHI_WAKE_DIAGNOSTICS
#define XIAOZHI_WAKE_DIAGNOSTICS 0
#endif
#ifndef XIAOZHI_SHOW_DEBUG_STATUS
#define XIAOZHI_SHOW_DEBUG_STATUS 1
#endif
#if XIAOZHI_WAKE_DIAGNOSTICS
#include "WakeDiagnostics.h"
#endif

#ifndef XIAOZHI_DEVICE_BOARD_TYPE
#if XIAOZHI_BOARD == OJ_ESP32S3_OJOY
#define XIAOZHI_DEVICE_BOARD_TYPE "oj-esp32s3-ojoy"
#else
#define XIAOZHI_DEVICE_BOARD_TYPE "esp32s3-tft-emotion-face"
#endif
#endif
#ifndef XIAOZHI_DEVICE_BOARD_NAME
#if XIAOZHI_BOARD == OJ_ESP32S3_OJOY
#define XIAOZHI_DEVICE_BOARD_NAME "OpenJumper OJoy"
#else
#define XIAOZHI_DEVICE_BOARD_NAME "ESP32-S3 TFT Emotion Face"
#endif
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
#define XIAOZHI_ENABLE_VOICE_BARGE_IN_DEFAULT 0
#endif
#ifndef XIAOZHI_ENABLE_WAKE_WORD_INTERRUPTION_DEFAULT
#define XIAOZHI_ENABLE_WAKE_WORD_INTERRUPTION_DEFAULT 1
#endif
#if XIAOZHI_ENABLE_WAKE_WORD_INTERRUPTION_DEFAULT && \
    XIAOZHI_ENABLE_VOICE_BARGE_IN_DEFAULT
#error "Select local wake-word interruption or server Realtime voice barge-in"
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
grokEyes eyes(tft, grokEyes::PLAIN_BASIC);
#if XIAOZHI_SHOW_DEBUG_STATUS
TFT_eSprite debugStatusSprite(&tft);
bool debugStatusReady = false;
const char* debugStartupPhase = "STARTING";
char debugLastError[48] = {};
uint32_t debugErrorAtMs = 0;
#endif
xiaozhi::ArduinoWebSocketTransport network_transport;

xiaozhi::AsyncTransportConfig makeTransportConfig() {
  xiaozhi::AsyncTransportConfig config;
  config.control_event_queue_depth = 4;
  config.receive_text_pool_depth = 1;
  config.receive_binary_pool_depth = 1;
  config.transmit_control_pool_depth = 3;
  config.transmit_audio_pool_depth = 4;
  config.maximum_text_bytes = 4096;
  config.maximum_binary_bytes = 1275 + 16;
  config.task_stack_in_psram = true;
  // The official service closes its audio channel after goodbye/inactivity.
  // Return to local wake listening instead of reopening that finished chat.
  config.reconnect_after_disconnect = false;
  return config;
}

xiaozhi::AsyncTransport transport(network_transport, makeTransportConfig());

I2sOpusAudioPort::Config makeAudioConfig() {
  I2sOpusAudioPort::Config config = xiaozhi_audio_board::makeConfig();
  config.inputTaskStackBytes = 4 * 1024;
  config.outputTaskStackBytes = 3 * 1024;
  config.wakeTaskStackBytes = 4 * 1024;
  config.captureLogIntervalPackets = 10;
  config.enableSpeechConditioning = true;
  config.speechGateRms = 300;
  config.speechTargetRms = 2200;
  config.speechMaximumGain = 4.0f;
  config.speechSilenceGain = 0.25f;
  config.speechStartPackets = 2;
  config.speechHoldMs = 420;
  config.enableWakeDetection = true;
  config.enableWakeWordInterruption =
      XIAOZHI_ENABLE_WAKE_WORD_INTERRUPTION_DEFAULT != 0;
  config.wakeModelPartition = "model";
  config.wakeModelKeyword = "wn9_nihaoxiaozhi_tts";
  config.defaultWakeWord = "你好小智";
  config.wakeDetectionMode = I2sOpusAudioPort::WakeDetectionMode::Balanced;
#if XIAOZHI_WAKE_DIAGNOSTICS
  config.wakeAudioObserver = wake_diagnostics::observe;
#endif
#if XIAOZHI_BOARD == OJ_ESP32S3_OJOY
  // Leave speaker headroom for acoustic wake-word interruption.
  config.hardware.es8311.outputVolumeDb = -12.0f;
  // Reduce playback echo at the ADC while keeping nearby speech audible.
  // Restore the normal 24 dB input gain when the speaker is muted.
  config.hardware.es8311.playbackMicrophoneGainDb = 12.0f;
  config.playbackMuteDelayMs = 150;
  config.enableWakeAec = true;
  config.wakeAecDuringPlaybackOnly = true;
  config.wakeDetectionMode = I2sOpusAudioPort::WakeDetectionMode::Aggressive;
  // Model metadata uses ~0.63. Favor the first utterance without requiring
  // a repeated word to build a stronger match; retain echo cancellation.
  config.wakeDetectionThreshold = 0.55f;
  config.wakeTaskStackBytes = 8 * 1024;
  // The right slot is playback reference, never an alternative microphone.
  config.captureChannel = I2sOpusAudioPort::CaptureChannel::Left;
  // The board's lower ADC gain needs a correspondingly lower uplink gate.
  config.speechGateRms = 40;
  config.speechMaximumGain = 16.0f;
#endif
  return config;
}

const I2sOpusAudioPort::Config audioConfig = makeAudioConfig();
I2sOpusAudioPort audioPort(audioConfig);

namespace {

constexpr char kWifiSsid[] = XIAOZHI_WIFI_SSID;
constexpr char kWifiPassword[] = XIAOZHI_WIFI_PASSWORD;

void* tlsPsramFirstCalloc(size_t count, size_t size) {
  if (count != 0 && size > SIZE_MAX / count) {
    return nullptr;
  }
  void* allocation =
      heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (allocation == nullptr) {
    allocation = heap_caps_calloc(count, size,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return allocation;
}

void tlsHeapCapsFree(void* allocation) {
  heap_caps_free(allocation);
}

constexpr uint16_t kExpressionTransitionMs = 600;
constexpr uint32_t kChatButtonDebounceMs = 40;
#if defined(CH13613_DRIVER)
constexpr int16_t kStatusVerticalInset = 70;  // Keep text inside the round panel.
#else
constexpr int16_t kStatusVerticalInset = 0;
#endif

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
uint32_t downlinkAudioFrames = 0;
uint32_t downlinkAudioBytes = 0;
uint32_t measuredWakeAudioHits = 0;
uint32_t measuredWakeCaptureHits = 0;
bool chatButtonReading = HIGH;
bool chatButtonStableState = HIGH;
uint32_t chatButtonChangedMs = 0;

void queueEmotion(xiaozhi::Emotion emotion, const std::string& rawName) {
  grokEyes::XiaozhiExpression displayExpression;
  if (!grokEyes::expressionFromName(rawName.c_str(), displayExpression)) {
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
                grokEyes::expressionName(displayExpression));
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
  // Reuse the eyes framebuffer while animation is suspended. Full-frame pushes
  // satisfy CH13613's 2x2 window alignment even for one-pixel text strokes.
  auto& screen = eyes.sprite();
  screen.fillSprite(TFT_BLACK);
  screen.setTextWrap(false, false);
  screen.setTextDatum(TC_DATUM);
  screen.setTextColor(TFT_RED, TFT_BLACK);
  screen.setTextSize(2);
  screen.drawString(title, tft.width() / 2, kStatusVerticalInset + 24, 1);
  screen.setTextColor(TFT_WHITE, TFT_BLACK);
  screen.setTextSize(1);
  screen.drawString(line1, tft.width() / 2, kStatusVerticalInset + 76, 1);
  if (line2 != nullptr) {
    screen.setTextColor(TFT_YELLOW, TFT_BLACK);
    screen.drawString(line2, tft.width() / 2, kStatusVerticalInset + 98, 1);
  }
  screen.setTextDatum(TL_DATUM);
  screen.pushSprite(0, 0);
}

void drawActivationCountdown(uint32_t seconds) {
  if (seconds == lastActivationCountdownSec) {
    return;
  }
  lastActivationCountdownSec = seconds;

  char text[48];
  snprintf(text, sizeof(text), "Checking registration in %lu s",
           static_cast<unsigned long>(seconds));
  const int16_t footerY = tft.height() - 20 - kStatusVerticalInset;
  auto& screen = eyes.sprite();
  screen.fillRect(0, footerY, tft.width(), 20, TFT_BLACK);
  screen.setTextDatum(TC_DATUM);
  screen.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  screen.setTextSize(1);
  screen.drawString(text, tft.width() / 2, footerY + 5, 1);
  screen.setTextDatum(TL_DATUM);
  screen.pushSprite(0, 0);
}

void showActivationScreen(const std::string& code) {
  const int16_t centerX = tft.width() / 2;
  const bool compact = tft.height() < 200;
  auto& screen = eyes.sprite();
  screen.fillSprite(TFT_BLACK);
  screen.setTextDatum(TC_DATUM);
  screen.setTextWrap(false, false);

  screen.setTextColor(TFT_CYAN, TFT_BLACK);
  screen.setTextSize(2);
  screen.drawString("XIAOZHI SETUP", centerX,
                    compact ? 4 : kStatusVerticalInset + 10, 1);

  screen.setTextColor(TFT_YELLOW, TFT_BLACK);
  screen.setTextSize(2);
  screen.drawString("NOT REGISTERED", centerX,
                    compact ? 26 : kStatusVerticalInset + 42, 1);

  screen.setTextColor(TFT_WHITE, TFT_BLACK);
  screen.setTextSize(compact ? 2 : 3);
  screen.drawString("xiaozhi.me", centerX,
                    compact ? 50 : kStatusVerticalInset + 78, 1);

  screen.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  screen.setTextSize(1);
  screen.drawString("ENTER ACTIVATION CODE", centerX,
                    compact ? 76 : kStatusVerticalInset + 118, 1);

  screen.setTextColor(TFT_GREEN, TFT_BLACK);
  screen.setTextSize(compact ? 3 : 4);
  screen.drawString(code.c_str(), centerX,
                    compact ? 90 : kStatusVerticalInset + 140, 1);
  screen.setTextDatum(TL_DATUM);
  screen.pushSprite(0, 0);

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

void drawDebugStatus(bool force = false) {
#if XIAOZHI_SHOW_DEBUG_STATUS
  if (!debugStatusReady || activationRequired || fatalScreen) { return; }
  static uint32_t lastUpdate = 0;
  static uint32_t lastFrame = 0;
  static uint32_t lastHits = 0;
  const uint32_t now = millis();
  const auto wake = audioPort.wakeStats();
  if (wake.detections != lastHits) {
    lastHits = wake.detections;
    force = true;
  }
  if (force || now - lastUpdate >= 250) {
    lastUpdate = now;
    const bool online = WiFi.status() == WL_CONNECTED;
    auto& panel = debugStatusSprite;
    panel.fillSprite(TFT_BLACK);
    panel.setTextDatum(TC_DATUM);
    panel.setTextColor(online ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
    char text[64];
    snprintf(text, sizeof(text), "WiFi %s | %s", online ? "ONLINE" : "OFFLINE",
             clientStarted ? runtime.stateName() : debugStartupPhase);
    panel.drawString(text, panel.width() / 2, 2, 2);
    panel.setTextColor(wake.enabled ? TFT_CYAN : TFT_LIGHTGREY, TFT_BLACK);
    const bool justHeard = wake.detections != 0 && now - wake.lastDetectionMs < 6000;
    snprintf(text, sizeof(text), "%s | HITS %lu | MIC %s",
             justHeard ? "HEARD!" : (wake.enabled ? "WAKE ON" : "WAKE OFF"),
             static_cast<unsigned long>(wake.detections),
             wake.capturing ? "SEND" : "LOCAL");
    if (!justHeard && wake.capturing) {
      snprintf(text, sizeof(text), "LISTENING: SPEAK NOW | HITS %lu",
               static_cast<unsigned long>(wake.detections));
    }
    panel.drawString(text, panel.width() / 2, 25, 2);
    panel.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(text, sizeof(text), "MIC %lu  REF %lu  AFE %lu",
             static_cast<unsigned long>(wake.capturing ? wake.captureRms : wake.rms),
             static_cast<unsigned long>(wake.referenceRms),
             static_cast<unsigned long>(wake.cleanRms));
    if (wake.enabled && !wake.aecActive) {
      snprintf(text, sizeof(text), "MIC %lu  AFE %lu  AEC OFF",
               static_cast<unsigned long>(wake.rms), static_cast<unsigned long>(wake.cleanRms));
    }
#if XIAOZHI_WAKE_DIAGNOSTICS
    if (wake_diagnostics::recording.load()) {
      snprintf(text, sizeof(text), "RECORDING %lu s",
               static_cast<unsigned long>((now - wake_diagnostics::startedMs) / 1000));
    }
    if (wake_diagnostics::dumping) {
      snprintf(text, sizeof(text), "EXPORTING AUDIO %u/2", wake_diagnostics::dumpStage + 1);
    }
#endif
    if (debugLastError[0] != '\0' && now - debugErrorAtMs < 10000) {
      panel.setTextColor(TFT_RED, TFT_BLACK);
      snprintf(text, sizeof(text), "ERROR: %.32s", debugLastError);
    }
    panel.drawString(text, panel.width() / 2, 48, 2);
    force = true;
  }
  // Redraw this separate, even-sized band after eye frames. It stays inside
  // the round AMOLED and is never cleared by grokEyes' full-sprite refresh.
  if (force || lastFrame != eyes.renderedFrames()) {
    lastFrame = eyes.renderedFrames();
    const int16_t y = (tft.height() - kStatusVerticalInset - 72) & ~1;
    debugStatusSprite.pushSprite(0, y);
  }
#else
  (void)force;
#endif
}

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
  const bool accepted = runtime.state() == xiaozhi::State::Speaking
                            ? runtime.requestAbortSpeaking()
                            : runtime.requestToggleChat();
  if (!accepted) {
    Serial.printf("[xiaozhi] dialogue toggle failed in state=%s\n",
                  runtime.stateName());
  }
}

void pollDialogueTriggers() {
  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
#if XIAOZHI_WAKE_DIAGNOSTICS
    if (wake_diagnostics::command(command)) { continue; }
#endif
    if (command == 't' || command == 'T') {
      toggleDialogue("serial");
    } else if (command == 'w' || command == 'W') {
      // Exercise the same urgent control path as WakeNet without requiring
      // an acoustic trigger. Real detection still happens on the wake task.
      const char* word = audioConfig.defaultWakeWord;
      const xiaozhi::State previousState = runtime.state();
      const bool accepted = runtime.requestWakeWordDetected(word, std::strlen(word));
      Serial.printf("[wake] serial trigger from state=%s accepted=%s\n",
                    xiaozhi::stateName(previousState), accepted ? "yes" : "no");
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
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxBufferSize(1024);
  Serial.setTxTimeoutMs(0);
#endif
  delay(250);
#if XIAOZHI_BOARD == OJ_ESP32S3_OJOY
  // Audio packet/PCM pools otherwise consume internal RAM in many 1-3 KiB
  // allocations. Keep task stacks, Wi-Fi/AES and DMA allocations internal;
  // let ordinary buffers >= 1 KiB prefer the board's 8 MiB PSRAM.
  if (psramFound()) {
    heap_caps_malloc_extmem_enable(1024);
  }
#endif
  pinMode(XIAOZHI_CHAT_BUTTON_PIN, INPUT_PULLUP);
  Serial.printf("[audio] profile=%s\n",
                I2sOpusAudioPort::Config::compiledProfileName());
  Serial.printf("[board] %s psram=%lu free_psram=%lu\n",
                XIAOZHI_DEVICE_BOARD_NAME,
                static_cast<unsigned long>(ESP.getPsramSize()),
                static_cast<unsigned long>(ESP.getFreePsram()));

#if defined(TFT_TE) && TFT_TE >= 0
  pinMode(TFT_TE, INPUT);
#endif
  tft.init();
  tft.setRotation(XIAOZHI_TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);

  eyesReady = eyes.begin(50);
  Serial.printf("[display] grokEyes=%s size=%dx%d\n",
                eyesReady ? "ready" : "failed", tft.width(), tft.height());
  if (!eyesReady) {
    tft.fillScreen(TFT_RED);
    Serial.println("[display] grokEyes sprite allocation failed");
    return;
  }
  eyes.setAutoBlink(true, 3000, 1600);
  eyes.setIdle(true, 1700);
  eyes.setExpression("neutral", 0);
#if XIAOZHI_SHOW_DEBUG_STATUS
  if (tft.height() >= 240) {
    debugStatusReady = debugStatusSprite.createSprite(tft.width() & ~1, 72) != nullptr;
    if (debugStatusReady) { eyes.setPosition(120, 104, 0); }
    Serial.printf("[display] debug status=%s\n", debugStatusReady ? "ready" : "unavailable");
    drawDebugStatus(true);
  }
#endif

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
    Serial.printf("[audio] ES8311 detected at I2C address 0x%02X; output=%.1f dB\n",
                  audioConfig.hardware.es8311.address,
                  audioConfig.hardware.es8311.outputVolumeDb);
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
  // This interactive example favors response time over modem power saving.
  WiFi.setSleep(false);
#if XIAOZHI_SHOW_DEBUG_STATUS
  debugStartupPhase = "WIFI CONNECTING";
  drawDebugStatus(true);
#endif
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
#if XIAOZHI_SHOW_DEBUG_STATUS
    debugStartupPhase = "WIFI FAILED";
#endif
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
#if XIAOZHI_SHOW_DEBUG_STATUS
  debugStartupPhase = "CHECKING ACCOUNT";
  drawDebugStatus(true);
#endif
  if (!xiaozhi::ArduinoOfficialService::configure(
          config, provisioning, provisioningError, serviceOptions)) {
    queueLocalEmotion("shocked");
    Serial.printf("[xiaozhi] provisioning failed: %s\n",
                  provisioningError.c_str());
#if XIAOZHI_SHOW_DEBUG_STATUS
    debugStartupPhase = "CONFIG FAILED";
#endif
    return;
  }
#if XIAOZHI_PROTOCOL_VERSION_OVERRIDE != 0
  config.protocol_version = XIAOZHI_PROTOCOL_VERSION_OVERRIDE;
#endif
  config.max_json_bytes = 4096;
  config.max_audio_payload_bytes = 1275;
  const int tlsAllocatorResult =
      mbedtls_platform_set_calloc_free(tlsPsramFirstCalloc, tlsHeapCapsFree);
  Serial.printf("[network] TLS PSRAM allocator: %s\n",
                tlsAllocatorResult == 0 ? "enabled" : "unavailable");
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
  Serial.printf("[wake] word interruption=%s\n",
                audioConfig.enableWakeWordInterruption ? "on" : "off");
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
    char pins[40];
    snprintf(pins, sizeof(pins), "CHECK SDA%d SCL%d",
             audioConfig.hardware.es8311.i2cSda,
             audioConfig.hardware.es8311.i2cScl);
    showSystemScreen("AUDIO ERROR", "ES8311 NOT FOUND",
                     pins);
#else
    showSystemScreen("AUDIO ERROR", "AUDIO NOT READY",
                     "CHECK AUDIO PINS");
#endif
    return;
  }
  network_transport.setCACertificate(
      xiaozhi::ArduinoOfficialService::rootCACertificate());

  xiaozhi::Callbacks callbacks;
  callbacks.on_event = onEvent;
  callbacks.on_wake_word = [](const std::string& wakeWord) {
    const auto wake = audioPort.wakeStats();
    queueLocalEmotion("thinking");
    Serial.printf("[wake] detected: %s at=%lu dispatch_ms=%lu hits=%lu\n",
                  wakeWord.c_str(), static_cast<unsigned long>(wake.lastDetectionMs),
                  static_cast<unsigned long>(millis() - wake.lastDetectionMs),
                  static_cast<unsigned long>(wake.detections));
  };
  callbacks.on_state_changed = [](xiaozhi::State, xiaozhi::State next) {
    Serial.printf("[xiaozhi] state=%s\n", xiaozhi::stateName(next));
    if (next == xiaozhi::State::Listening) {
      queueLocalEmotion("thinking");
    } else if (next == xiaozhi::State::Idle) {
      queueLocalEmotion("neutral");
    }
  };
  callbacks.on_capture = [](bool enabled, const xiaozhi::AudioFormat& format) {
    const auto wake = audioPort.wakeStats();
    if (enabled && wake.detections != measuredWakeCaptureHits) {
      measuredWakeCaptureHits = wake.detections;
      Serial.printf("[wake-latency] capture_ms=%lu\n",
                    static_cast<unsigned long>(millis() - wake.lastDetectionMs));
    }
    Serial.printf("[xiaozhi] capture=%s format=%lu Hz/%u ch/%u ms\n",
                  enabled ? "on" : "off",
                  static_cast<unsigned long>(format.sample_rate),
                  format.channels, format.frame_duration_ms);
  };
  callbacks.on_audio_meta = [](const xiaozhi::AudioFrameMeta& meta) {
    const auto wake = audioPort.wakeStats();
    if (wake.detections != measuredWakeAudioHits) {
      measuredWakeAudioHits = wake.detections;
      Serial.printf("[wake-latency] first_downlink_ms=%lu\n",
                    static_cast<unsigned long>(millis() - wake.lastDetectionMs));
    }
    ++downlinkAudioFrames;
    downlinkAudioBytes += static_cast<uint32_t>(meta.opus_bytes);
  };
  callbacks.on_error = [](xiaozhi::ErrorCode code, const std::string& message) {
#if XIAOZHI_SHOW_DEBUG_STATUS
    snprintf(debugLastError, sizeof(debugLastError), "%s", xiaozhi::errorName(code));
    debugErrorAtMs = millis();
#endif
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
  xiaozhi::ClientRuntimeConfig runtimeConfig;
  if (!runtime.begin(config, callbacks, runtimeConfig)) {
    showSystemScreen("STARTUP ERROR", "AUDIO OR CLIENT FAILED",
                     "CHECK SERIAL LOG");
    return;
  }
  clientStarted = true;
  Serial.println(
      "[xiaozhi] ready; say 你好小智 (also interrupts speech), press BOOT, "
      "or send 't'; 'w' tests the wake control path");
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
#if XIAOZHI_WAKE_DIAGNOSTICS
    wake_diagnostics::loop();
    if (wake_diagnostics::dumping) {
      drawDebugStatus();
      delay(1);
      return;
    }
#endif
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
  drawDebugStatus();

  const uint32_t now = millis();
  if (now - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = now;
    Serial.printf(
        "[heartbeat] eyes=%s wifi=%d client=%s emotion=%u,%s "
        "display=%u,%s audio=%lu/%lu heap=%lu frames=%lu free_psram=%lu\n",
        eyesReady ? "ready" : "failed", static_cast<int>(WiFi.status()),
        runtime.stateName(), static_cast<unsigned>(currentEmotion),
        xiaozhi::emotionName(currentEmotion),
        static_cast<unsigned>(eyes.xiaozhiExpression()),
        currentEmotionName.c_str(),
        static_cast<unsigned long>(downlinkAudioFrames),
        static_cast<unsigned long>(downlinkAudioBytes),
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(eyes.renderedFrames()),
        static_cast<unsigned long>(ESP.getFreePsram()));
    const auto wake = audioPort.wakeStats();
    Serial.printf("[wake-status] on=%u capture=%u reads=%lu partial=%lu frames=%lu "
                  "hits=%lu peak=%lu rms=%lu ch=%c ref=%lu clean=%lu aec=%lu aec_us=%lu aec_on=%u clip=%lu mic_db=%.0f\n",
                  wake.enabled, wake.capturing,
                  static_cast<unsigned long>(wake.reads),
                  static_cast<unsigned long>(wake.partialReads),
                  static_cast<unsigned long>(wake.frames),
                  static_cast<unsigned long>(wake.detections),
                  static_cast<unsigned long>(wake.peak),
                  static_cast<unsigned long>(wake.rms), wake.channel,
                  static_cast<unsigned long>(wake.referenceRms),
                  static_cast<unsigned long>(wake.cleanRms),
                  static_cast<unsigned long>(wake.aecFrames),
                  static_cast<unsigned long>(wake.maximumAecUs), wake.aecActive,
                  static_cast<unsigned long>(wake.clippedReads), wake.microphoneGainDb);
  }

  delay(1);
}
