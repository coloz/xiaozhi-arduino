#pragma once

// Compile-time audio hardware profiles. Only the selected backend and its
// optional headers are compiled into the sketch translation unit.
#define XIAOZHI_AUDIO_PROFILE_ES8311 1
#define XIAOZHI_AUDIO_PROFILE_I2S_DUPLEX 2
#define XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX 3
#define XIAOZHI_AUDIO_PROFILE_PDM_I2S 4
#define XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC 5

// Common module-pair aliases. Their pins and electrical details still come
// from the selected audio-board preset or the application's custom audio setup.
#define XIAOZHI_AUDIO_PROFILE_INMP441_MAX98357A \
  XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
#define XIAOZHI_AUDIO_PROFILE_MSM261_MAX98357A \
  XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
#define XIAOZHI_AUDIO_PROFILE_MP34DT05_MAX98357A \
  XIAOZHI_AUDIO_PROFILE_PDM_I2S
