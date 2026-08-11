#pragma once

#include "AudioProfile.h"

// Select an audio-only board preset in the .ino before including Xiaozhi.h:
//   #define XIAOZHI_AUDIO_BOARD XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX
//   #include <Xiaozhi.h>
#define XIAOZHI_AUDIO_BOARD_CUSTOM 0
#define XIAOZHI_AUDIO_BOARD_OJ_ESP32S3_BASIC 1
#define XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX 2

#ifndef XIAOZHI_AUDIO_BOARD
#define XIAOZHI_AUDIO_BOARD XIAOZHI_AUDIO_BOARD_OJ_ESP32S3_BASIC
#endif

#if XIAOZHI_AUDIO_BOARD == XIAOZHI_AUDIO_BOARD_OJ_ESP32S3_BASIC
#include "OjEsp32S3Basic.h"
#elif XIAOZHI_AUDIO_BOARD == XIAOZHI_AUDIO_BOARD_NULLLAB_AI_VOX
#include "NullLabAiVox.h"
#elif XIAOZHI_AUDIO_BOARD == XIAOZHI_AUDIO_BOARD_CUSTOM
// Define BOARD_AUDIO_* and XIAOZHI_AUDIO_PROFILE before including this header.
#else
#error "Unsupported XIAOZHI_AUDIO_BOARD"
#endif

#ifndef XIAOZHI_AUDIO_PROFILE
#error "The selected audio board must define XIAOZHI_AUDIO_PROFILE"
#endif

#include "BoardDefaults.h"
