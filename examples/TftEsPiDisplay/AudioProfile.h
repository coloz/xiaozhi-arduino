#pragma once

// Compile-time audio hardware profiles. Define XIAOZHI_AUDIO_PROFILE to one of
// these values before including I2sOpusAudioPort.h. Only the selected backend
// and its optional headers are compiled into the sketch translation unit.
#define XIAOZHI_AUDIO_PROFILE_ES8311 1
#define XIAOZHI_AUDIO_PROFILE_I2S_DUPLEX 2
#define XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX 3
#define XIAOZHI_AUDIO_PROFILE_PDM_I2S 4
#define XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC 5

// Common module-pair aliases. The I2S microphone aliases default to a 24-bit
// sample in a 32-bit slot; callers may override slot and shift in Config.
#define XIAOZHI_AUDIO_PROFILE_INMP441_MAX98357A \
  XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
#define XIAOZHI_AUDIO_PROFILE_MSM261_MAX98357A \
  XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
#define XIAOZHI_AUDIO_PROFILE_MP34DT05_MAX98357A \
  XIAOZHI_AUDIO_PROFILE_PDM_I2S
