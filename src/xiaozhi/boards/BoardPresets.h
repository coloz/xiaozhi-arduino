#pragma once

#include "AudioProfile.h"

// Select an audio-only board preset in the .ino before including Xiaozhi.h:
//   #define XIAOZHI_BOARD NULLLAB_AI_VOX3
//   #include <Xiaozhi.h>
#define CUSTOM_BOARD 0
#define OJ_ESP32S3_BASIC 1
#define NULLLAB_AI_VOX 2
#define NULLLAB_AI_VOX3 3

#ifndef XIAOZHI_BOARD
#define XIAOZHI_BOARD OJ_ESP32S3_BASIC
#endif

#if XIAOZHI_BOARD == OJ_ESP32S3_BASIC
#include "OjEsp32S3Basic.h"
#elif XIAOZHI_BOARD == NULLLAB_AI_VOX
#include "NullLabAiVox.h"
#elif XIAOZHI_BOARD == NULLLAB_AI_VOX3
#include "NullLabAiVox3.h"
#elif XIAOZHI_BOARD == CUSTOM_BOARD
// Define BOARD_AUDIO_* and XIAOZHI_AUDIO_PROFILE before including this header.
#else
#error "Unsupported XIAOZHI_BOARD"
#endif

#ifndef XIAOZHI_AUDIO_PROFILE
#error "The selected audio board must define XIAOZHI_AUDIO_PROFILE"
#endif

#include "BoardDefaults.h"
