# Xiaozhi Arduino

English | [简体中文](README.md)

This is an Arduino implementation of the Xiaozhi project.
Rather than copying the entire original ESP-IDF project into Arduino, it retains the protocols and state-management logic required for voice sessions while turning display, board, and algorithm implementations into optional adaptation layers.

## Design Goals

- Maintain compatibility with the Xiaozhi WebSocket protocol v1/v2/v3, Opus frame formats, session state, and MCP JSON-RPC.
- Avoid dependencies on LVGL, fonts, emoji, sound-effect assets, cameras, LEDs, specific development boards, or ESP-SR.
- Avoid global singletons so that one sketch can create, replace, or test multiple independent objects.
- Validate the size, type, and boundaries of all network and audio input.
- Keep TLS secure by default: `wss://` connections must provide a trusted CA. If no certificate is available, a custom `Transport` must be used instead.

## Core and Extension Boundaries

| Included in the library core | Provided by Arduino/board-level extensions |
|---|---|
| Session state machine and events | LVGL/U8g2/display UI |
| WebSocket v1/v2/v3 encoding and decoding | I2S and specific audio codec chips |
| hello/listen/abort/STT/TTS/LLM messages | PCM-to-Opus/Opus-to-PCM codecs and resampling |
| MCP tool registration, argument validation, and pagination | ESP-SR, WakeNet, VAD, and AEC |
| OTA/activation configuration parsing and HTTP retrieval | Cameras, LEDs, buttons, and power management |
| ESP32 MAC address, persistent UUID, and firmware version | Fonts, emoji, OGG files, and asset partitions |
| Injectable `Transport` / `EncodedAudioPort` | MQTT+UDP transport extensions |

Arduino recursively compiles every source file under a library's `src/` directory, so optional modules cannot avoid their dependencies merely by remaining unused. They are therefore kept entirely outside the core `src/` directory. The core still uses Opus as its wire format, but it does not force users to adopt any particular Opus or I2S library.

## Installation

Requirements:

- ESP32 Arduino Core 3.x; the verified baseline is 3.3.11 (ESP-IDF 5.5.5).
- ArduinoJson 7.x (required; Library Manager installs it according to `library.properties`).
- ArduinoWebsockets 0.5.4 or later (install only when using the built-in `ArduinoWebSocketTransport`).

A sketch that uses the built-in WebSocket adapter should explicitly include
`#include <ArduinoWebsockets.h>` as shown in the examples. This allows the Arduino
builder to discover the optional dependency and pass its include path to Xiaozhi.
ArduinoWebsockets does not need to be installed or included when this adapter is not used.

Copy this directory to `libraries/Xiaozhi` in your Arduino sketchbook, or choose **Sketch > Include Library > Add .ZIP Library** in the Arduino IDE.

Display libraries are not core dependencies. Install them separately as needed and open the corresponding example:

- `examples/U8g2Display`: U8g2 2.36+ for monochrome OLED displays.
- `examples/TftEsPiDisplay`: TFT_eSPI 2.5+; configure its `User_Setup` before flashing.
- `examples/LvglDisplay`: LVGL 9.x + TFT_eSPI 2.5+; LVGL manages widgets, while TFT_eSPI only flushes pixels to the display. The example includes a minimal `lv_conf.h`, so a clean installation does not require changes to the LVGL library directory. This configuration enables only the Label widget used by the example and disables themes, complex drawing, Flex, and Grid. Enable additional features as needed for other widgets, rounded corners, or shadows, and keep an eye on the remaining space in the default application partition.

These three display examples demonstrate UI integration and independent installation only. They do not include microphone, speaker, or Opus implementations, and they do not make the display libraries dependencies of Xiaozhi. Their default fonts cover ASCII only. To display Chinese STT/TTS text, choose a font with the required Chinese glyphs in the corresponding display library.

All three examples subscribe only to `Callbacks`; no display header is included anywhere under this library's `src/` directory. A project that does not use a display therefore does not need any of the libraries above.

## Minimal Usage

```cpp
#include <Xiaozhi.h>

xiaozhi::ArduinoWebSocketTransport transport;
xiaozhi::Client client(transport);

void setup() {
  // Connect to Wi-Fi first. Leaving the service configuration empty uses the
  // official Xiaozhi provisioning service directly.
  xiaozhi::ClientConfig config;
  xiaozhi::ProvisioningResult provisioning;
  std::string error;
  xiaozhi::OfficialServiceOptions serviceOptions;

  // To use a self-hosted compatible provisioning service, uncomment the next
  // line and replace the URL:
  // serviceOptions.provisioning_url = "https://your-server.example/xiaozhi/ota/";

  if (!xiaozhi::ArduinoOfficialService::configure(
          config, provisioning, error, serviceOptions)) {
    Serial.println(error.c_str());
    return;
  }
  transport.setCACertificate(
      xiaozhi::ArduinoOfficialService::rootCACertificate());

  xiaozhi::Callbacks callbacks;
  callbacks.on_event = [](const xiaozhi::Event& event) {
    // Forward STT/TTS/emotion/alert events to Serial, LVGL, or any other UI.
  };
  callbacks.on_error = [](xiaozhi::ErrorCode, const std::string& message) {
    Serial.println(message.c_str());
  };
  client.begin(config, callbacks);
}

void loop() {
  client.loop();
}
```

Call `client.startListening()` to establish a session. Send encoded upstream frames with `client.sendAudio()`; downstream frames are returned through `callbacks.on_audio`. The recommended approach is to implement `EncodedAudioPort` and call `client.attachAudioPort(&port)` before `begin()`, so that the audio extension is responsible only for Opus encoding/decoding and hardware queues.

For compatibility with the original 2.4.0 implementation, `protocol_version` defaults to 1. Set it to 2 or 3 only when the server explicitly supports that version. All three versions have the same JSON semantics but use different binary Opus header formats.

## Concurrency Contract

`Client::loop()` is the sole writer for the session. A custom `Transport` should dispatch callbacks from its `loop()` method rather than invoking them directly from a hidden RTOS task. Hardware interrupts and audio tasks should first write to their own bounded queues, which are then passed to `Client` by `loop()`. The state machine itself is protected by a lock and notifies listeners only after releasing it, preventing reentrant deadlocks.

Do not call control APIs such as `startListening()`, `closeSession()`, or `sendMcp()` directly from a user callback. These calls fail to prevent recursive state transitions. Instead, have the callback set a flag in the sketch, then perform the control action after `client.loop()` returns. Calling `end()` from a callback is safely deferred.

## Security and Resource Limits

- `ClientConfig` can limit JSON and Opus payload sizes; the defaults are 8192 and 4096 bytes, respectively.
- The v2/v3 binary headers use explicit big-endian reads and writes. Truncated data, forged lengths, and oversized payloads are rejected without modifying the receive buffer.
- MCP arguments are validated for type, required fields, default values, and integer ranges. Error messages are escaped by ArduinoJson.
- Remote calls to `user_only` MCP tools are denied by default. They can run only after the local UI explicitly authorizes them through `setUserToolAuthorizer()`.
- The caller supplies the `NetworkClient` used for provisioning HTTP requests. For HTTPS, pass a `NetworkClientSecure` configured with a CA certificate.
- The built-in WebSocket adapter does not provide an option to disable TLS certificate validation.

The built-in adapter also enforces a hard 16384-byte limit in its callback. However, ArduinoWebsockets 0.5.x assembles a complete message before entering that callback, so this check cannot prevent the dependency from making the initial allocation for a malicious server message. When connecting to an untrusted endpoint, build ArduinoWebsockets with `_WS_CONFIG_MAX_MESSAGE_SIZE=16384`, or inject a custom `Transport` that enforces the limit while reading.

## Current Compatibility Scope

The core targets ESP32, ESP32-S3, ESP32-C3, ESP32-C5, ESP32-C6, and ESP32-P4. WebSocket sessions, protocols, MCP, provisioning parsing, and the encoded Opus frame interface belong to this library. Specific audio hardware, Opus implementations, offline wake-word detection, UI, and MQTT+UDP remain separate extension boundaries. See [MIGRATION.md](MIGRATION.md) and [TESTING.md](TESTING.md) for details.
