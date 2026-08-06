/*
 * TftEsPiDisplay 示例
 *
 * 演示由 TFT_eSPI 状态界面、服务配置、I2S 音频及 Opus 编解码组成的完整
 * 语音对话流程。示例不绑定开发板厂家；移植时请修改 BoardConfig.h，并可
 * 通过 I2sOpusAudioPort::Config 调整音频参数或接入其他 Codec。
 */

#include <Arduino.h>

#include "BoardConfig.h"
#include "BoardAudioConfig.h"

#include <TFT_eSPI.h>  // Uses the sketch-local User_Setup.h for this board.
#include <WiFi.h>
#include <ArduinoWebsockets.h>  // Makes the optional transport dependency explicit.
#if XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR
#include <ESP_SR.h>             // WakeNet and its model-image build hook.
#endif
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
#include <Wire.h>
#include <EspressifEs8311.h>    // Audio dependencies used by the implementation.
#endif
#include <EspressifOpus.h>
#include <Xiaozhi.h>

#if __has_include("Secrets.h")
#include "Secrets.h"
#endif
// Keep the optional audio implementation in this sketch translation unit.
// The .impl.h suffix prevents Arduino builders from compiling it a second time.
#include "I2sOpusAudioPort.impl.h"

// Protocol/UI callbacks need more than Arduino's default 8 KiB. Opus now runs
// on its dedicated codec task, so the loop no longer needs a 48 KiB stack.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Optional dependencies for this example only:
//   TFT_eSPI >= 2.5 and ArduinoWebsockets >= 0.5.4
// Display and audio wiring lives in BoardConfig.h. Most audio behavior can be
// changed through I2sOpusAudioPort::Config without editing its implementation.
TFT_eSPI tft;
xiaozhi::ArduinoWebSocketTransport transport;

const I2sOpusAudioPort::Config audioConfig =
    xiaozhi_audio_board::makeConfig();
I2sOpusAudioPort audioPort(audioConfig);

namespace {
#ifndef XIAOZHI_WIFI_SSID
#define XIAOZHI_WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef XIAOZHI_WIFI_PASSWORD
#define XIAOZHI_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif
constexpr char kWifiSsid[] = XIAOZHI_WIFI_SSID;
constexpr char kWifiPassword[] = XIAOZHI_WIFI_PASSWORD;

// The library uses Xiaozhi's official provisioning service by default. This
// legacy fully-custom block is intentionally disabled; users who need a custom
// service can use the short serviceOptions override in setup() below.
#if 0
constexpr char kProvisioningUrl[] = "https://api.tenclass.net/xiaozhi/ota/";

// DigiCert Global Root G2 currently anchors api.tenclass.net. Provisioning uses
// Arduino-ESP32's full built-in CA bundle; this PEM is used by ArduinoWebsockets.
constexpr char kOfficialRootCa[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBhMQswCQYDVQQG
EwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cuZGlnaWNlcnQuY29tMSAw
HgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUx
MjAwMDBaMGExCzAJBgNVBAYTAlVTMRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3
dy5kaWdpY2VydC5jb20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkq
hkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI2/Ou8jqJ
kTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx1x7e/dfgy5SDN67sH0NO
3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQq2EGnI/yuum06ZIya7XzV+hdG82MHauV
BJVJ8zUtluNJbd134/tJS7SsVQepj5WztCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyM
UNGPHgm+F6HmIcr9g+UQvIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQAB
o0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV5uNu
5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY1Yl9PMWLSn/pvtsr
F9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4NeF22d+mQrvHRAiGfzZ0JFrabA0U
WTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NGFdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBH
QRFXGU7Aj64GxJUTFy8bJZ918rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/
iyK5S9kJRaTepLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----)PEM";

extern const uint8_t x509CrtBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509CrtBundleEnd[] asm("_binary_x509_crt_bundle_end");
#endif

std::string statusLine = "starting";
std::string emotionLine = "neutral";
std::string roleLine = "system";
std::string messageLine = "Xiaozhi Arduino";
bool displayDirty = true;
uint32_t lastDrawMs = 0;
uint32_t lastSpeakingAudioMs = 0;
bool chatButtonReading = HIGH;
bool chatButtonStableState = HIGH;
uint32_t chatButtonChangedMs = 0;

constexpr uint32_t kChatButtonDebounceMs = 40;
constexpr uint32_t kSpeakingSilenceTimeoutMs = 12000;

bool hasPlaceholderNetworkConfig() {
  return strcmp(kWifiSsid, "YOUR_WIFI_SSID") == 0 ||
         strcmp(kWifiPassword, "YOUR_WIFI_PASSWORD") == 0;
}

void logConfiguredNetworkScan() {
  Serial.println("[wifi] scanning for configured SSID");
  const int count = WiFi.scanNetworks(false, true);
  bool found = false;
  for (int index = 0; index < count; ++index) {
    if (WiFi.SSID(index) == kWifiSsid) {
      found = true;
      Serial.printf("[wifi] target visible, RSSI=%ld dBm channel=%ld\n",
                    static_cast<long>(WiFi.RSSI(index)),
                    static_cast<long>(WiFi.channel(index)));
    }
  }
  if (!found) {
    Serial.printf("[wifi] target SSID is not visible (%d network(s) scanned)\n", count);
  }
  WiFi.scanDelete();
}

void connectConfiguredWifi() {
  WiFi.mode(WIFI_STA);
  uint32_t attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    ++attempt;
    Serial.printf("[wifi] connecting, attempt %lu\n", static_cast<unsigned long>(attempt));
    WiFi.begin(kWifiSsid, kWifiPassword);
    if (WiFi.waitForConnectResult(20000) == WL_CONNECTED) {
      return;
    }

    statusLine = "wifi retry";
    roleLine = "system";
    messageLine = "WiFi unavailable; retrying";
    displayDirty = true;
    Serial.printf("[wifi] connection failed, status=%d\n", WiFi.status());
    WiFi.disconnect(false, false);
    delay(250);
    logConfiguredNetworkScan();
    delay(5000);
  }
}

#if 0
bool synchronizeClock() {
  configTime(0, 0, "ntp.aliyun.com", "pool.ntp.org");
  const uint32_t deadline = millis() + 15000;
  while (time(nullptr) < 1700000000 && static_cast<int32_t>(deadline - millis()) > 0) {
    delay(100);
  }
  return time(nullptr) >= 1700000000;
}

std::string buildSystemInfoJson(const std::string& deviceId,
                                const std::string& clientId) {
  JsonDocument document;
  document["version"] = 2;
  document["language"] = "zh-CN";
  document["flash_size"] = ESP.getFlashChipSize();
  document["psram_size"] = ESP.getPsramSize();
  document["minimum_free_heap_size"] = ESP.getMinFreeHeap();
  document["mac_address"] = deviceId;
  document["uuid"] = clientId;
  document["chip_model_name"] = "esp32s3";

  esp_chip_info_t chipInfo{};
  esp_chip_info(&chipInfo);
  JsonObject chip = document["chip_info"].to<JsonObject>();
  chip["model"] = chipInfo.model;
  chip["cores"] = chipInfo.cores;
  chip["revision"] = chipInfo.revision;
  chip["features"] = chipInfo.features;

  JsonObject application = document["application"].to<JsonObject>();
  application["name"] = "xiaozhi-arduino";
  application["version"] = xiaozhi::Esp32Identity::firmwareVersion();
  application["compile_time"] = __DATE__ "T" __TIME__ "Z";
  application["idf_version"] = ESP.getSdkVersion();
  application["elf_sha256"] = "";
  document["partition_table"].to<JsonArray>();
  document["ota"]["label"] = "app";
  document["board"]["type"] = BOARD_TYPE;
  document["board"]["name"] = BOARD_NAME;

  std::string output;
  serializeJson(document, output);
  return output;
}

bool fetchOfficialConfiguration(const std::string& deviceId,
                                const std::string& clientId,
                                xiaozhi::ProvisioningResult& result,
                                std::string& error) {
  NetworkClientSecure tls;
  tls.setCACertBundle(x509CrtBundleStart,
                      static_cast<size_t>(x509CrtBundleEnd - x509CrtBundleStart));

  xiaozhi::ProvisioningRequest request;
  request.url = kProvisioningUrl;
  request.device_id = deviceId;
  request.client_id = clientId;
  request.user_agent = BOARD_TYPE "/2.4.0";
  request.language = "zh-CN";
  request.body_json = buildSystemInfoJson(deviceId, clientId);
  request.activation_version = 1;
  request.connect_timeout_ms = 15000;
  request.read_timeout_ms = 15000;
  return xiaozhi::ArduinoProvisioningClient::fetch(tls, request, result, error);
}
#endif

void setMessage(const std::string& role, const std::string& message) {
  const size_t roleLength = role.size() < 64 ? role.size() : 64;
  const size_t messageLength = message.size() < 256 ? message.size() : 256;
  roleLine.assign(role.data(), roleLength);
  messageLine.assign(message.data(), messageLength);
  displayDirty = true;
}

void redraw() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.print("Xiaozhi");
  tft.drawFastHLine(8, 31, tft.width() - 16, TFT_DARKGREY);

  tft.setTextSize(1);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(8, 42);
  tft.printf("state: %s", statusLine.c_str());
  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  tft.setCursor(8, 62);
  tft.printf("emotion: %s", emotionLine.c_str());
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(8, 82);
  tft.printf("%s:", roleLine.c_str());
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 100);
  tft.setTextWrap(true, false);
  tft.print(messageLine.c_str());
}

void onEvent(const xiaozhi::Event& event) {
  switch (event.type) {
    case xiaozhi::EventType::Stt:
      setMessage("user", event.text);
      Serial.printf("[xiaozhi] STT: %s\n", event.text.c_str());
      break;
    case xiaozhi::EventType::TtsSentence:
      setMessage("assistant", event.text);
      Serial.printf("[xiaozhi] TTS: %s\n", event.text.c_str());
      break;
    case xiaozhi::EventType::Emotion:
      emotionLine.assign(event.emotion.data(),
                         std::min<size_t>(event.emotion.size(), 64));
      Serial.printf("[xiaozhi] emotion: %s\n", emotionLine.c_str());
      break;
    case xiaozhi::EventType::Alert:
      setMessage(event.status, event.text);
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
    Serial.printf("[xiaozhi] dialogue toggle from %s failed in state=%s\n",
                  source, runtime.stateName());
  }
}

void pollDialogueTriggers() {
  std::string wakeWord;
  if (audioPort.consumeWakeWord(wakeWord)) {
    Serial.printf("[wake] detected: %s\n", wakeWord.c_str());
    if (!runtime.ready() || runtime.state() != xiaozhi::State::Idle ||
        !runtime.requestWakeWordDetected(wakeWord)) {
      Serial.printf("[wake] could not enter dialogue from state=%s\n",
                    runtime.stateName());
    }
  }

  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    if (command == 't' || command == 'T') {
      toggleDialogue("serial");
    }
  }

  const uint32_t now = millis();
  const bool reading = digitalRead(BOARD_CHAT_BUTTON);
  if (reading != chatButtonReading) {
    chatButtonReading = reading;
    chatButtonChangedMs = now;
  }
  if (reading != chatButtonStableState &&
      now - chatButtonChangedMs >= kChatButtonDebounceMs) {
    chatButtonStableState = reading;
    if (chatButtonStableState == BOARD_CHAT_BUTTON_ACTIVE_LEVEL) {
      toggleDialogue("BOOT button");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);
  pinMode(BOARD_CHAT_BUTTON, INPUT_PULLUP);
  Serial.printf("[board] %s\n", BOARD_NAME);
  Serial.printf("[display] ST7789 %dx%d, SCLK=%d MOSI=%d DC=%d CS=%d\n",
                BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, BOARD_LCD_SCLK,
                BOARD_LCD_MOSI, BOARD_LCD_DC, BOARD_LCD_CS);

  tft.init();
  tft.setRotation(BOARD_LCD_ROTATION);
  redraw();
  Serial.printf("[display] initialized: %dx%d\n", tft.width(), tft.height());

  const auto& audioOutput = audioConfig.hardware.output;
  Serial.printf("[audio] profile=%s OUT port=%d MCLK=%d BCLK=%d WS=%d DATA=%d\n",
                I2sOpusAudioPort::Config::compiledProfileName(),
                audioOutput.port, audioOutput.mclk, audioOutput.bclk,
                audioOutput.ws, audioOutput.data);
#if XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM
  const auto& audioInput = audioConfig.hardware.pdmInput;
  Serial.printf("[audio] IN PDM port=%d CLK=%d DATA=%d\n",
                audioInput.port, audioInput.clock, audioInput.data);
#else
  const auto& audioInput = audioConfig.hardware.input;
  Serial.printf("[audio] IN I2S port=%d MCLK=%d BCLK=%d WS=%d DATA=%d\n",
                audioInput.port, audioInput.mclk, audioInput.bclk,
                audioInput.ws, audioInput.data);
#endif
  if (xiaozhi_audio_board::probe(audioConfig)) {
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
    Serial.printf("[audio] ES8311 detected at I2C address 0x%02X\n",
                  audioConfig.hardware.es8311.address);
#else
    Serial.println("[audio] direct audio pins configured; runtime probe unavailable");
#endif
  } else {
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
    Serial.printf("[audio] ES8311 not detected at I2C address 0x%02X\n",
                  audioConfig.hardware.es8311.address);
#else
    Serial.println("[audio] selected audio hardware probe failed");
#endif
  }

  if (hasPlaceholderNetworkConfig()) {
    statusLine = "configure wifi";
    setMessage("system", "Replace WiFi and server placeholders");
    redraw();
    Serial.println("[wifi] placeholders detected; set kWifiSsid/kWifiPassword");
    return;
  }

  connectConfiguredWifi();
  Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());

  IPAddress resolvedAddress;
  if (WiFi.hostByName("api.tenclass.net", resolvedAddress) == 1) {
    Serial.printf("[network] DNS api.tenclass.net -> %s\n",
                  resolvedAddress.toString().c_str());
  } else {
    Serial.println("[network] DNS lookup failed");
  }
  NetworkClient connectivityProbe;
  if (connectivityProbe.connect("api.tenclass.net", 80, 5000)) {
    Serial.println("[network] internet TCP connectivity OK");
    connectivityProbe.stop();
  } else {
    Serial.println("[network] internet TCP connectivity failed");
  }

  statusLine = "provisioning";
  redraw();
  xiaozhi::ClientConfig config;
  xiaozhi::ProvisioningResult provisioning;
  std::string provisioningError;

  // No server settings are required: the library defaults to Xiaozhi's official
  // provisioning service. Uncomment and edit the override only for a compatible
  // custom provisioning service.
  xiaozhi::OfficialServiceOptions serviceOptions;
  serviceOptions.board_type = BOARD_TYPE;
  serviceOptions.board_name = BOARD_NAME;
  /*
  serviceOptions.provisioning_url = "https://your-server.example/xiaozhi/ota/";
  */

  Serial.println("[network] synchronizing clock and fetching official configuration");
  if (!xiaozhi::ArduinoOfficialService::configure(
          config, provisioning, provisioningError, serviceOptions)) {
    statusLine = "provision failed";
    setMessage("system", provisioningError);
    redraw();
    Serial.printf("[xiaozhi] official provisioning failed: %s\n",
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
  Serial.println("[network] clock synchronized");
  Serial.printf("[xiaozhi] device=%s client=%s\n",
                config.device_id.c_str(), config.client_id.c_str());
  Serial.printf("[xiaozhi] official WebSocket received, protocol v%u, token present=%s\n",
                config.protocol_version,
                provisioning.websocket.token.empty() ? "no" : "yes");
  Serial.printf("[audio] server AEC=%s voice barge-in=%s\n",
                config.enable_server_aec ? "on" : "off",
                config.enable_voice_barge_in ? "on" : "off");
  if (aecRequested && !config.enable_server_aec) {
    Serial.println("[audio] protocol has no AEC timestamp; using AutoStop to prevent echo self-interruption");
  }
  if (provisioning.activation.present && !provisioning.activation.code.empty()) {
    statusLine = "activation";
    setMessage("xiaozhi.me", provisioning.activation.code);
    redraw();
    Serial.printf("[xiaozhi] activation: %s code=%s\n",
                  provisioning.activation.message.c_str(),
                  provisioning.activation.code.c_str());
  }

  transport.setCACertificate(xiaozhi::ArduinoOfficialService::rootCACertificate());

  xiaozhi::Callbacks callbacks;
  callbacks.on_state_changed = [](xiaozhi::State, xiaozhi::State next) {
    statusLine = xiaozhi::stateName(next);
    displayDirty = true;
    Serial.printf("[xiaozhi] state=%s\n", xiaozhi::stateName(next));
    const bool lowLatencySession = runtime.sessionReady() ||
                                   next == xiaozhi::State::Connecting ||
                                   next == xiaozhi::State::Listening ||
                                   next == xiaozhi::State::Speaking;
    // Modem sleep is useful after the audio channel closes but can add
    // DTIM-sized jitter even in ManualStop's idle-yet-connected state.
    WiFi.setSleep(!lowLatencySession);
    audioPort.setWakeDetectionEnabled(next == xiaozhi::State::Idle);
    if (next == xiaozhi::State::Listening) {
      lastSpeakingAudioMs = 0;
    } else if (next == xiaozhi::State::Speaking) {
      lastSpeakingAudioMs = millis();
    } else if (next == xiaozhi::State::Idle) {
      lastSpeakingAudioMs = 0;
    }
  };
  callbacks.on_event = onEvent;
  callbacks.on_audio = [](const xiaozhi::AudioFrame&) {
    lastSpeakingAudioMs = millis();
  };
  callbacks.on_capture = [](bool enabled, const xiaozhi::AudioFormat& format) {
    Serial.printf("[xiaozhi] capture=%s format=%lu Hz/%u ch/%u ms\n",
                  enabled ? "on" : "off", static_cast<unsigned long>(format.sample_rate),
                  format.channels, format.frame_duration_ms);
  };
  callbacks.on_error = [](xiaozhi::ErrorCode code, const std::string& message) {
    statusLine = "error";
    setMessage("system", message);
    Serial.printf("[xiaozhi] error (%u): %s\n", static_cast<unsigned int>(code),
                  message.c_str());
  };

  if (!client.attachAudioPort(&audioPort)) {
    Serial.println("[audio] failed to attach the I2S/Opus audio port");
    return;
  }
  if (runtime.begin(config, callbacks)) {
    Serial.println("[xiaozhi] client started");
    statusLine = "idle";
    if (!provisioning.activation.present || provisioning.activation.code.empty()) {
      setMessage("system", "Press BOOT to talk (serial: t)");
    }
    Serial.println("[xiaozhi] idle; press BOOT or send 't' to start a dialogue");
  } else {
    Serial.println("[xiaozhi] client initialization failed");
  }
}

void loop() {
  runtime.loop();
  pollDialogueTriggers();
  const uint32_t now = millis();
  if (runtime.ready() && runtime.state() == xiaozhi::State::Speaking &&
      lastSpeakingAudioMs != 0 &&
      now - lastSpeakingAudioMs >= kSpeakingSilenceTimeoutMs &&
      audioPort.playbackIdle()) {
    Serial.println("[xiaozhi] speaking watchdog: no TTS audio, closing stale session");
    lastSpeakingAudioMs = 0;
    runtime.requestCloseSession();
  }
  if (displayDirty && now - lastDrawMs >= 50) {
    displayDirty = false;
    lastDrawMs = now;
    redraw();
  }
  delay(1);
}
