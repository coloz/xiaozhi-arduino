/*
 * Direct .ino audio configuration for NULLLAB AI-VOX3 (ES8311).
 * Edit makeAudioConfig() to use your own wiring, gain, and sample rate.
 * Select ESP32S3 Dev Module, OPI PSRAM, 16 MB Flash, and Huge APP partition.
 * Dependencies: ArduinoJson, ArduinoWebsockets, EspressifOpus, EspressifEs8311.
 * Fill in Wi-Fi below. Activation codes and conversation text appear on Serial.
 * Press BOOT or send 't' on Serial to toggle a conversation.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <Wire.h>
#include <EspressifEs8311.h>
#include <EspressifOpus.h>
#include <Xiaozhi.h>
// Select the audio backend by header; no board or pin macros are needed.
#include <xiaozhi/audio/Es8311Audio.h>

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

constexpr char kWifiSsid[] = "YOUR_WIFI_SSID";
constexpr char kWifiPassword[] = "YOUR_WIFI_PASSWORD";
constexpr int kChatButtonPin = 0;

I2sOpusAudioPort::Config makeAudioConfig() {
  // Use the factory so the hardware fields below take effect. Plain Config{}
  // retains the legacy flat-field configuration for older applications.
  auto config = I2sOpusAudioPort::Config::forCompiledProfile();

  // ES8311 uses I2S0 with shared clocks and 16-bit stereo slots.
  auto& output = config.hardware.output;
  output.port = 0;
  output.sampleRate = 16000;
  output.mclk = 11;
  output.bclk = 10;
  output.ws = 8;
  output.data = 7;

  auto& input = config.hardware.input;
  input = output;
  input.data = 9;
  config.captureChannel = I2sOpusAudioPort::CaptureChannel::Left;

  auto& codec = config.hardware.es8311;
  codec.wire = &Wire;
  codec.i2cSda = 13;
  codec.i2cScl = 12;
  codec.i2cFrequency = 400000;
  codec.address = 0x18;  // Wire uses the 7-bit address.
  codec.noDacReference = false;
  codec.microphoneGainDb = 30.0f;
  codec.outputVolumeDb = -12.0f;

  // This example uses BOOT/Serial, so no ESP-SR model partition is needed.
  config.enableWakeDetection = false;
  return config;
}

// The audio port copies the configuration. Make changes before construction.
I2sOpusAudioPort audioPort(makeAudioConfig());
xiaozhi::ArduinoWebSocketTransport networkTransport;
xiaozhi::AsyncTransport transport(networkTransport);
xiaozhi::Client client(transport);
xiaozhi::ClientRuntime runtime(client);
bool clientStarted = false;
uint32_t nextProvisioningMs = 0;

void startClient() {
  xiaozhi::ClientConfig config;
  xiaozhi::ProvisioningResult provisioning;
  xiaozhi::OfficialServiceOptions options;
  options.board_type = "nulllab-ai-vox3-arduino";
  options.board_name = "NULLLAB AI-VOX3 (manual audio config)";
  std::string error;
  const bool configured = xiaozhi::ArduinoOfficialService::configure(
      config, provisioning, error, options);
  // An activation-only response has no WebSocket settings yet; configure()
  // returns false while still preserving the activation code in provisioning.
  if (provisioning.activation.present && !provisioning.activation.code.empty()) {
    Serial.printf("[activation] code=%s message=%s\n",
                  provisioning.activation.code.c_str(),
                  provisioning.activation.message.c_str());
    nextProvisioningMs = millis() + 30000;
    return;
  }
  if (!configured) {
    Serial.printf("[setup] %s\n", error.c_str());
    nextProvisioningMs = millis() + 30000;
    return;
  }

  networkTransport.setCACertificate(
      xiaozhi::ArduinoOfficialService::rootCACertificate());
  xiaozhi::Callbacks callbacks;
  callbacks.on_state_changed = [](xiaozhi::State, xiaozhi::State next) {
    Serial.printf("[state] %s\n", xiaozhi::stateName(next));
  };
  callbacks.on_event = [](const xiaozhi::Event& event) {
    Serial.printf("[event] %s\n", event.text.c_str());
  };
  callbacks.on_error = [](xiaozhi::ErrorCode code, const std::string& message) {
    Serial.printf("[error:%s] %s\n", xiaozhi::errorName(code), message.c_str());
  };

  // The audio port was attached in setup(); Runtime services it from here.
  clientStarted = runtime.begin(config, callbacks);
  if (!clientStarted) {
    Serial.println("[setup] client/audio initialization failed");
    nextProvisioningMs = millis() + 30000;
  }
}

void toggleChat() {
  if (!clientStarted || !runtime.ready()) return;
  if (runtime.state() == xiaozhi::State::Speaking) {
    runtime.requestAbortSpeaking();
  } else {
    runtime.requestToggleChat();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(kChatButtonPin, INPUT_PULLUP);
  if (!client.attachAudioPort(&audioPort)) {
    Serial.println("[setup] could not attach audio port");
    return;
  }
  WiFi.begin(kWifiSsid, kWifiPassword);
  Serial.println("Connecting to Wi-Fi; fill in kWifiSsid/kWifiPassword first.");
}

void loop() {
  const uint32_t now = millis();
  if (!clientStarted && WiFi.status() == WL_CONNECTED &&
      static_cast<int32_t>(now - nextProvisioningMs) >= 0) {
    startClient();
  }
  runtime.loop();

  // Debounce BOOT; one press requests one toggle.
  static bool lastReading = HIGH;
  static bool stableReading = HIGH;
  static uint32_t changedMs = 0;
  const bool reading = digitalRead(kChatButtonPin);
  if (reading != lastReading) {
    lastReading = reading;
    changedMs = now;
  }
  if (reading != stableReading && now - changedMs >= 30) {
    stableReading = reading;
    if (reading == LOW) toggleChat();
  }
  if (Serial.available() && Serial.read() == 't') toggleChat();
  delay(1);
}
