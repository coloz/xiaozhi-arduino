#pragma once

#define XIAOZHI_ARDUINO_VERSION_MAJOR 2
#define XIAOZHI_ARDUINO_VERSION_MINOR 4
#define XIAOZHI_ARDUINO_VERSION_PATCH 0
#define XIAOZHI_ARDUINO_VERSION "2.4.0"

// Audio-board support is opt-in. Only microphone, speaker, codec, and audio-bus
// settings are selected here; display libraries keep their own pin setup.
//   #define XIAOZHI_AUDIO_BOARD XIAOZHI_AUDIO_BOARD_OJ_ESP32S3_BASIC
#if defined(XIAOZHI_AUDIO_BOARD)
#include "xiaozhi/boards/BoardPresets.h"
#endif

#include "xiaozhi/ArduinoWebSocketTransport.h"
#include "xiaozhi/AsyncTransport.h"
#include "xiaozhi/Audio.h"
#include "xiaozhi/Client.h"
#include "xiaozhi/ClientRuntime.h"
#include "xiaozhi/Clock.h"
#include "xiaozhi/Emotion.h"
#include "xiaozhi/Esp32Identity.h"
#include "xiaozhi/McpServer.h"
#include "xiaozhi/Protocol.h"
#include "xiaozhi/Provisioning.h"
#include "xiaozhi/StateMachine.h"
#include "xiaozhi/Transport.h"
#include "xiaozhi/Types.h"

// Keep the optional third-party audio implementation out of normal builds.
// Defining XIAOZHI_AUDIO_BOARD makes the umbrella header a complete audio-board
// entry point for the sketch translation unit.
#if defined(XIAOZHI_AUDIO_BOARD)
#include "xiaozhi/audio/AudioBoard.h"
#endif
