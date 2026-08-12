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
| MCP registration, validation, pagination, and server-AEC protocol | ESP-SR, WakeNet, VAD, and device-side AFE/AEC |
| OTA/activation configuration parsing and HTTP retrieval | Cameras, LEDs, buttons, and power management |
| ESP32 MAC address, persistent UUID, and firmware version | Fonts, emoji, OGG files, and asset partitions |
| Injectable `Transport` / `EncodedAudioPort` | MQTT+UDP transport extensions |

Arduino recursively compiles `.cpp` files under a library's `src/` directory, so the optional audio implementation is stored as opt-in headers under `src/xiaozhi/audio`. A normal application that does not define `XIAOZHI_AUDIO_BOARD` pulls in no Opus, codec, or ESP-SR dependency. Once selected, `Xiaozhi.h` includes that audio implementation in the sketch translation unit.

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
- `examples/TftEsPiDisplay`: TFT_eSPI 2.5+; configure its `User_Setup`, then edit Wi-Fi directly at the top of the `.ino` before flashing.
- `examples/TftEmotionFace`: TftRobotEyes 1.1+ and TFT_eSPI 2.5+; displays the server's exact emotion strings as full-color animated eyes and prints serial state with stable enum values.
- `examples/LvglDisplay`: LVGL 9.x + TFT_eSPI 2.5+; LVGL manages widgets, while TFT_eSPI only flushes pixels to the display. The example includes a minimal `lv_conf.h`, so a clean installation does not require changes to the LVGL library directory. This configuration enables only the Label widget used by the example and disables themes, complex drawing, Flex, and Grid. Enable additional features as needed for other widgets, rounded corners, or shadows, and keep an eye on the remaining space in the default application partition.

All five display examples demonstrate UI integration, but they do not have identical scope. `U8g2Display`, `U8g2RobotEyesEmotion`, and `LvglDisplay` are display-only integrations without audio. `TftEsPiDisplay` and `TftEmotionFace` are complete voice terminals with microphone capture, Opus encoding/decoding, and speaker playback. Display headers remain on the example side; the optional audio implementation lives under `src/xiaozhi/audio` and is included only when an audio board is selected. Their default fonts cover ASCII only. To display Chinese STT/TTS text, choose a font with the required Chinese glyphs in the corresponding display library.

The two complete voice examples select their hardware path through a compile-time audio profile, so only the chosen backend is compiled:

- full-duplex ES8311 codec;
- I2S microphone and speaker sharing clocks;
- microphone and speaker on separate I2S controllers, such as an INMP441 or MSM261 paired with a MAX98357A;
- a PDM microphone paired with a MAX98357A;
- an application-provided custom codec.

See [AUDIO_PROFILES.md](AUDIO_PROFILES.md) for profile selection, required pins, and hardware constraints. The display layer in all five examples still receives states and events only through `Callbacks`; no display header is included under this library's `src/`. A project without a display therefore needs none of the display libraries above.

## Audio Board Presets

Microphone, speaker, codec, I2S, and audio-I2C settings for common boards live
under `src/xiaozhi/boards`. Select one value at the top of the `.ino`, before
including `Xiaozhi.h`; no `BoardConfig.h` or copied audio pin list is required:

```cpp
#define XIAOZHI_AUDIO_BOARD XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX
#include <Xiaozhi.h>
```

The same selection can be supplied as a build flag, for example
`-DXIAOZHI_AUDIO_BOARD=XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX`. Built-in presets are:

| Selection | Board | Audio hardware |
| --- | --- | --- |
| `XIAOZHI_AUDIO_BOARD_OJ_ESP32S3_BASIC` | OpenJumper ESP32 AIOT Basic (`oj_esp32s3basic`) | ES8311 on shared full-duplex I2S |
| `XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX` | first-generation NULLLAB AI VOX | SPH0645 microphone plus an independent I2S amplifier |

The complete examples explicitly default to
`XIAOZHI_AUDIO_BOARD_OJ_ESP32S3_BASIC`. The AI VOX preset follows the `ai_vox`
repository's `examples/ai_vox_board`; it is not the pin-incompatible AI-VOX3.

Audio presets define no display, backlight, or button pins. TFT_eSPI cannot
accept pins in its constructor, so configure the installed library's
`User_Setup.h` or its build flags; the examples no longer carry a display setup
header. U8g2 clock and data pins stay beside the constructor in the single
`.ino`. Changing an audio preset never changes the display configuration.

For a new audio board, select `XIAOZHI_AUDIO_BOARD_CUSTOM` and define the
required `BOARD_AUDIO_*` and `XIAOZHI_AUDIO_PROFILE` macros before including
`xiaozhi/boards/BoardPresets.h`.

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

xiaozhi::ArduinoWebSocketTransport network_transport;
xiaozhi::AsyncTransport transport(network_transport);
xiaozhi::Client client(transport);
xiaozhi::ClientRuntime runtime(client);

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
  network_transport.setCACertificate(
      xiaozhi::ArduinoOfficialService::rootCACertificate());

  xiaozhi::Callbacks callbacks;
  callbacks.on_event = [](const xiaozhi::Event& event) {
    // Forward STT/TTS/emotion/alert events to Serial, LVGL, or any other UI.
  };
  callbacks.on_error = [](xiaozhi::ErrorCode, const std::string& message) {
    Serial.println(message.c_str());
  };
  if (runtime.begin(config, callbacks)) {
    runtime.requestStartListening(xiaozhi::ListeningMode::AutoStop);
  }
}

void loop() {
  // Only user callbacks run here. WebSocket, timeouts, and audio uplink run on
  // the dedicated Xiaozhi task.
  runtime.loop();
}
```

`ClientRuntime` is recommended for Arduino applications. It keeps the single-writer `Client`, WebSocket polling, protocol timeouts, and `EncodedAudioPort::loop()` on a dedicated FreeRTOS task. Normal requests use a bounded queue, while stop, abort, wake, and local mute use a fixed urgent channel that runs before normal commands. A temporarily slow display refresh, sensor read, or third-party library on Arduino's `loopTask` therefore does not stop the Xiaozhi data path or local interruption controls.

The built-in network examples also wrap `ArduinoWebSocketTransport` in `AsyncTransport`. DNS, TCP/TLS, WebSocket polling, and socket sends run on a separate network task; Client/Runtime sees only bounded RX/TX pools. Its default priority 1 is below Runtime's 2, so local controls can preempt even a lower-level busy wait. Control text has priority over audio; a saturated audio TX pool rejects and counts only that packet instead of closing the session. A disconnect advances the connection generation, discards old-session RX/TX, and preserves desired-connected retry with exponential backoff from 250 ms to 5 s. `close()` cancels desired state immediately; if a blocking synchronous connect returns later, its old-generation result is discarded. `connect_timeout_ms` explicitly classifies timed-out attempts. ArduinoWebsockets' synchronous DNS/TLS call itself cannot be forcibly terminated, but it no longer occupies Runtime. Configure pools, size limits, task placement, and backoff with `AsyncTransportConfig`. Do not access the wrapped Transport from another task after the wrapper has started.

`runtime.loop()` only dispatches a bounded number of user callbacks (four by default). Delaying it postpones UI/log callbacks but does not stop protocol or attached-audio servicing. Callbacks are routed by semantics: state and capture each overwrite a latest-value slot; wake, event, and error use a separate bounded reliable queue; audio observers use a best-effort queue that can drop observations without crowding critical events or dropping actual playback. Runtime uses fixed payload pools for large callback and command data. `runtime.stats()` exposes coalescing, typed drops, pool exhaustion, and queue/pool high-water marks. Do not call the underlying `client` while its Runtime is active. Code that needs fully custom scheduling or deterministic synchronous tests can omit Runtime and continue to drive `Client::loop()` directly.

The default Runtime uses an 8192-byte stack, priority 2, and core 0. With `AsyncTransport` and the built-in I2S audio port it no longer polls every 2 ms: normal commands, urgent controls, wake events, Transport RX, audio uplink, and playback progress send a task notification. With no event, Runtime waits for the minimum Client handshake/channel deadline or playback-watchdog deadline, and sleeps indefinitely when neither exists. `AsyncTransport` waits for its reconnect timer on the network task and notifies Runtime only when a connection event is ready. A synchronous Transport or legacy audio extension that does not implement `EncodedAudioPort::eventDriven()` automatically retains the `poll_interval_ms` compatibility fallback; `idle_poll_interval_ms` is used only to recheck hardware that is still draining after an expired playback watchdog. Arduino normally runs `loopTask` on core 1 on dual-core parts, while core 0 remains valid on single-core chips. Prefer `on_audio_meta` when the application only needs downlink timestamps, Opus byte counts, and format information. Set `on_audio` only when the application must inspect the Opus payload, because that observer copies data from a fixed pool across tasks.

`Event` retains parsed STT, TTS, emotion, alert, and other semantic fields without copying the raw protocol JSON by default. To inspect the original message for debugging or a custom protocol, explicitly set `ClientConfig::include_raw_event_json = true` and set `maximum_raw_event_json_bytes` to a strict value no larger than `max_json_bytes`. Oversized events still deliver their semantic fields, but leave `event.json` empty.

The recommended audio integration is to implement `EncodedAudioPort` and call `client.attachAudioPort(&port)` before `runtime.begin()`. Advanced applications without an audio port can still use the synchronous `Client` API for manually supplied uplink frames.

`EncodedAudioPort` also has borrowed `play(const AudioFrameView&)`, move-aware playback, `cancelPlayback()`, and `queuedPlaybackMs()` hooks; existing extensions remain source compatible. The Opus pointer in a borrowed view is valid only for the duration of `play()`. A custom port that decodes asynchronously must copy it synchronously into its own bounded buffer before returning. Implementing cancellation lets a user abort, a new TTS turn, session closure, or a transport disconnect invalidate buffered speech immediately instead of playing stale audio.

The complete `TftEsPiDisplay` audio example runs I2S capture, Opus codec work, and I2S output on separate tasks; the Runtime task forwards a bounded batch of encoded uplink packets before WebSocket polling. Its hot queues are preallocated fixed rings. PCM and downlink Opus storage are allocated at startup, while uplink Opus containers stay in a fixed pool after their first encoded packet. A downlink borrowed view is copied once from the transport directly into the decode pool, with no per-packet allocation after warmup. The default downlink pool permits 42 queued packets of at most 1275 bytes and is independently bounded by the 2400 ms latency limit; tune these with `maximumDecodePackets`, `maximumDownlinkOpusBytes`, and `maximumDecodeQueueMs`. The sketch keeps Wi-Fi power saving disabled during a session. Device-side ESP-SR AFE/AEC remains a board extension that needs a real playback-reference path and hardware validation.

For compatibility with 2.4.0, `protocol_version` defaults to 1. Core `ClientConfig` requests server AEC and voice barge-in by default, but the complete TFT examples enter `Realtime` only when provisioning actually selects protocol v2, whose uplink frames carry played-downlink timestamps. Hardware testing showed that the official v1 endpoint mistakes speaker echo for barge-in and stops replies after a few syllables, so v1/v3 automatically use `AutoStop`. Button, serial, and `abortSpeaking()` interruption remain available.

The complete TFT examples expose compile-time switches directly in the `.ino`:

```cpp
#define XIAOZHI_ENABLE_SERVER_AEC_DEFAULT 1
#define XIAOZHI_ENABLE_VOICE_BARGE_IN_DEFAULT 1
#define XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC 0
```

Or override one Client at runtime:

```cpp
xiaozhi::ClientConfig config;
config.enable_server_aec = true;
config.enable_voice_barge_in = true;
```

Set both fields to `false` for half-duplex `AutoStop`. Set `XIAOZHI_ALLOW_UNTIMESTAMPED_SERVER_AEC` to 1 only for a custom v1/v3 server or device path that has been acoustically verified; do not force it with the official v1 endpoint. Manual interruption remains independent of these switches.

## Concurrency Contract

With `ClientRuntime`, only its service task accesses the underlying `Client`. Arduino tasks and user callbacks must use `runtime.request*()`, read Runtime's atomic state snapshots, or call `runtime.loop()`; do not mix in `client.loop()` or Client control calls. `runtime.end()` waits for service-task exit and Client/audio teardown. If a finite timeout returns `false`, the Runtime is still valid: retry later and do not destroy its Transport, Client, or audio port early.

`Client::loop()` is the sole writer for the session. A synchronous custom `Transport` may dispatch callbacks from `connect/send/close` or `loop()`, but every call must run on the Client task and never enter Client from a hidden RTOS task. `AsyncTransport` is the controlled exception: its network task copies data, while callbacks are dispatched only from the `loop()` call made by Client. Protocol text produced by a callback is kept in a small bounded queue and sent after the active `connect/loop/send` returns, so a synchronous Transport need not support recursive `send`; an overlong callback chain closes the session instead of self-triggering forever. `sendText/sendBinary` must consume or copy their input before returning and must not retain the pointer. `close()` must be idempotent and establish a generation barrier so an old connection cannot dispatch after a new one begins. Hardware interrupts and audio tasks should first write to their own bounded queues, which the audio port's `loop()` then passes to Client. An event-driven audio port should also call the installed `AudioEventNotifier` after publishing uplink work or advancing playback; the notifier only wakes Runtime and must never enter Client across tasks. An audio port's `end()` must stop its workers and all uplink callbacks before returning. The state machine itself is protected by a lock and notifies listeners only after releasing it, preventing reentrant deadlocks.

When using Client directly, do not call control APIs such as `startListening()`, `closeSession()`, or `sendMcp()` from a user callback. These calls fail to prevent recursive state transitions. Set a sketch flag and act after `client.loop()` returns. With Runtime, callbacks may instead submit `runtime.request*()` operations.

## Security and Resource Limits

- `ClientConfig` can limit JSON and Opus payload sizes; the defaults are 8192 and 4096 bytes, respectively.
- The v2/v3 binary headers use explicit big-endian reads and writes. Truncated data, forged lengths, and oversized payloads are rejected without modifying the receive buffer.
- MCP arguments are validated for type, required fields, default values, and integer ranges. Error messages are escaped by ArduinoJson.
- Remote calls to `user_only` MCP tools are denied by default. They can run only after the local UI explicitly authorizes them through `setUserToolAuthorizer()`.
- The caller supplies the `NetworkClient` used for provisioning HTTP requests. For HTTPS, pass a `NetworkClientSecure` configured with a CA certificate.
- The built-in WebSocket adapter does not provide an option to disable TLS certificate validation.

The built-in adapter switches ArduinoWebsockets 0.5.x to fragment notifications and reassembles them in its own 16384-byte total-size buffer. It closes immediately on an oversized or invalid fragment sequence, so the dependency cannot continue aggregating an unbounded whole message. In ArduinoWebsockets 0.5.4, changing the policy does not rebuild the existing StreamBuilder, so the first fragmented message of each new client still briefly has a second dependency-owned copy; an individual frame is also allocated before the callback. When connecting to an untrusted endpoint, continue to build ArduinoWebsockets with `_WS_CONFIG_MAX_MESSAGE_SIZE=16384`, or inject a custom `Transport` that enforces the limit while reading.

## Current Compatibility Scope

The core targets ESP32, ESP32-S3, ESP32-C3, ESP32-C5, ESP32-C6, and ESP32-P4. WebSocket sessions, protocols, MCP, provisioning parsing, and the encoded Opus frame interface belong to this library. Specific audio hardware, Opus implementations, offline wake-word detection, UI, and MQTT+UDP remain separate extension boundaries. See [PERFORMANCE.md](PERFORMANCE.md), [MIGRATION.md](MIGRATION.md), and [TESTING.md](TESTING.md) for details.
