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
| Session state machine, events, and standard emotion enum | LVGL/U8g2/display UI |
| WebSocket v1/v2/v3 encoding and decoding | I2S and specific audio codec chips |
| hello/listen/abort/STT/TTS/LLM messages | PCM-to-Opus/Opus-to-PCM codecs and resampling |
| MCP tool registration, argument validation, and pagination | ESP-SR, WakeNet, VAD, and AEC |
| OTA/activation configuration parsing and HTTP retrieval | Cameras, LEDs, buttons, and power management |
| ESP32 MAC address, persistent UUID, and firmware version | Fonts, emoji, OGG files, and asset partitions |
| Injectable `Transport` / `EncodedAudioPort` | MQTT+UDP transport extensions |

Arduino recursively compiles every source file under a library's `src/` directory, so optional modules cannot avoid their dependencies merely by remaining unused. The core `src/` therefore remains free of optional audio-hardware, Opus, and ESP-SR dependencies. The core still uses Opus as its wire format without forcing a particular Opus or I2S library; the complete examples include the selected audio implementation in the sketch translation unit through a compile-time profile.

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
- `examples/U8g2RobotEyesEmotion`: U8g2RobotEyes 1.3+ and U8g2 2.36+; displays the server's exact emotion strings as 128x64 Xiaozhi eyes.
- `examples/TftEsPiDisplay`: TFT_eSPI 2.5+; configure its `User_Setup`, then copy `Secrets.example.h` to the Git-ignored `Secrets.h` and fill in Wi-Fi before flashing.
- `examples/TftEmotionFace`: TftRobotEyes 1.1+ and TFT_eSPI 2.5+; displays the server's exact emotion strings as full-color animated eyes and prints serial state with stable enum values.
- `examples/LvglDisplay`: LVGL 9.x + TFT_eSPI 2.5+; LVGL manages widgets, while TFT_eSPI only flushes pixels to the display. The example includes a minimal `lv_conf.h`, so a clean installation does not require changes to the LVGL library directory. This configuration enables only the Label widget used by the example and disables themes, complex drawing, Flex, and Grid. Enable additional features as needed for other widgets, rounded corners, or shadows, and keep an eye on the remaining space in the default application partition.

All five display examples demonstrate UI integration, but they do not have identical scope. `U8g2Display`, `U8g2RobotEyesEmotion`, and `LvglDisplay` are display-only integrations without audio. `TftEsPiDisplay` and `TftEmotionFace` are complete voice terminals with microphone capture, Opus encoding/decoding, and speaker playback. All display headers and these optional audio dependencies remain on the example side and do not become dependencies of the core `src/`. Their default fonts cover ASCII only. To display Chinese STT/TTS text, choose a font with the required Chinese glyphs in the corresponding display library.

The two complete voice examples select their hardware path through a compile-time audio profile, so only the chosen backend is compiled:

- full-duplex ES8311 codec;
- I2S microphone and speaker sharing clocks;
- microphone and speaker on separate I2S controllers, such as an INMP441 or MSM261 paired with a MAX98357A;
- a PDM microphone paired with a MAX98357A;
- an application-provided custom codec.

See [AUDIO_PROFILES.md](AUDIO_PROFILES.md) for profile selection, required pins, and hardware constraints. The display layer in all five examples still receives states and events only through `Callbacks`; no display header is included under this library's `src/`. A project without a display therefore needs none of the display libraries above.

## Emotion Events

A server message such as `{"type":"llm","emotion":"happy"}` produces an
`EventType::Emotion` event. The core parses standard names into
`event.emotion_type` (`xiaozhi::Emotion`) and also retains the raw protocol value
in `event.emotion` for source compatibility and custom server emotions:

```cpp
callbacks.on_event = [](const xiaozhi::Event& event) {
  if (event.type != xiaozhi::EventType::Emotion) return;

  Serial.printf("emotion=%s code=%u\n", event.emotion.c_str(),
                static_cast<unsigned>(event.emotion_type));
  switch (event.emotion_type) {
    case xiaozhi::Emotion::Happy:
      // Show a happy face.
      break;
    case xiaozhi::Emotion::Sad:
      // Show a sad face.
      break;
    case xiaozhi::Emotion::Unknown:
      // event.emotion still contains the custom server value.
      break;
    default:
      break;
  }
};
```

Use `emotionFromName()` and `emotionName()` for application-side conversion in
either direction. Enum numbers are stable for a local serial protocol; the
Xiaozhi wire protocol continues to use lowercase string names.
The official table defines 21 standard values: `neutral`, `happy`, `laughing`,
`funny`, `sad`, `angry`, `crying`, `loving`, `embarrassed`, `surprised`,
`shocked`, `thinking`, `winking`, `cool`, `relaxed`, `delicious`, `kissy`,
`confident`, `sleepy`, `silly`, and `confused`.

`TftEmotionFace` and `U8g2RobotEyesEmotion` both call
`eyes.setExpression(event.emotion.c_str())` directly, with no second mapping
table. Both use the official provisioning service. Copy each example's
`Secrets.example.h` to the Git-ignored `Secrets.h` and fill in Wi-Fi settings.
Without that file, the examples automatically cycle all 21 emotions offline so
the screen and animation path can be tested first.

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

`EncodedAudioPort` also has optional move-aware playback, `cancelPlayback()`, and `queuedPlaybackMs()` hooks; existing extensions remain source compatible. Implementing cancellation lets a user abort, a new TTS turn, session closure, or a transport disconnect invalidate buffered speech immediately instead of playing stale audio.

The complete `TftEsPiDisplay` audio example runs I2S capture, Opus codec work, and I2S output on separate tasks; `loop()` forwards only a bounded number of encoded uplink packets and services uplink before a WebSocket poll that may process a batch. Its four hot queues are preallocated fixed rings and its PCM/Opus pools are reused. Capture downsampling uses a generated 15-tap Q15 filter, 48-to-24 kHz playback uses a streaming 31-tap Q15 filter, and upsampling uses cross-packet causal interpolation with one source-sample delay. Both directions remain latency-bounded and drop oldest data; the default downlink cap is reduced from 2400 ms to 600 ms. Capture, wake, uplink, and playback generations prevent stale data from crossing a turn; a user abort mutes locally first and reopens the microphone only after the playback tail is idle. The sketch keeps Wi-Fi power saving disabled while a session remains connected to reduce first-packet and conversational jitter. These policies apply to the ESP32-S3 full-audio example. The core still does not enable AFE/AEC implicitly; server AEC or a board-specific ESP-SR AFE must be validated against the actual acoustic path.

For compatibility with the original 2.4.0 implementation, `protocol_version` defaults to 1. Set it to 2 or 3 only when the server explicitly supports that version. All three versions have the same JSON semantics but use different binary Opus header formats. `enable_server_aec` is accepted only with protocol v2; the full audio port then associates the timestamp of a downlink packet that has actually reached I2S with a following uplink frame, and `toggleChat()` and wake-word entry default to `Realtime`. Without server AEC, the wire timestamp for uplink audio is always zero.

## Concurrency Contract

`Client::loop()` is the sole writer for the session. A custom `Transport` may dispatch callbacks synchronously from `connect/send/close` or from its `loop()`, but every call must run on the same task as Client and never enter Client from a hidden RTOS task. Protocol text produced by a callback is kept in a small bounded queue and sent after the active `connect/loop/send` returns, so a Transport need not support recursive `send`; an overlong synchronous callback chain closes the session instead of self-triggering forever. `sendText/sendBinary` must consume or copy their input before returning and must not retain the pointer. `close()` must be idempotent, quiesce old callbacks even when `connected()==false`, and accept a request from a synchronous callback (destruction may be deferred until the active call unwinds). Hardware interrupts and audio tasks should first write to their own bounded queues, which the audio port's `loop()` then passes to Client; an audio port's `end()` must stop its workers and all uplink callbacks before returning. The state machine itself is protected by a lock and notifies listeners only after releasing it, preventing reentrant deadlocks.

Do not call control APIs such as `startListening()`, `closeSession()`, or `sendMcp()` directly from a user callback. These calls fail to prevent recursive state transitions. Instead, have the callback set a flag in the sketch, then perform the control action after `client.loop()` returns. Calling `end()` from a callback is safely deferred.

## Security and Resource Limits

- `ClientConfig` can limit JSON and Opus payload sizes; the defaults are 8192 and 4096 bytes, respectively.
- The v2/v3 binary headers use explicit big-endian reads and writes. Truncated data, forged lengths, and oversized payloads are rejected without modifying the receive buffer.
- MCP arguments are validated for type, required fields, default values, and integer ranges. Error messages are escaped by ArduinoJson.
- Remote calls to `user_only` MCP tools are denied by default. They can run only after the local UI explicitly authorizes them through `setUserToolAuthorizer()`.
- The caller supplies the `NetworkClient` used for provisioning HTTP requests. For HTTPS, pass a `NetworkClientSecure` configured with a CA certificate.
- The built-in WebSocket adapter does not provide an option to disable TLS certificate validation.

The built-in adapter switches ArduinoWebsockets 0.5.x to fragment notifications and reassembles them in its own 16384-byte total-size buffer. It closes immediately on an oversized or invalid fragment sequence, so the dependency cannot continue aggregating an unbounded whole message. In ArduinoWebsockets 0.5.4, changing the policy does not rebuild the existing StreamBuilder, so the first fragmented message of each new client still briefly has a second dependency-owned copy; an individual frame is also allocated before the callback. When connecting to an untrusted endpoint, continue to build ArduinoWebsockets with `_WS_CONFIG_MAX_MESSAGE_SIZE=16384`, or inject a custom `Transport` that enforces the limit while reading.

## Current Compatibility Scope

The core targets ESP32, ESP32-S3, ESP32-C3, ESP32-C5, ESP32-C6, and ESP32-P4. WebSocket sessions, protocols, MCP, provisioning parsing, and the encoded Opus frame interface belong to this library. Specific audio hardware, Opus implementations, offline wake-word detection, UI, and MQTT+UDP remain separate extension boundaries. See [MIGRATION.md](MIGRATION.md) and [TESTING.md](TESTING.md) for details.
