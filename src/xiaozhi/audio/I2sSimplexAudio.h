#pragma once

// Separate I2S buses; fill Config::forCompiledProfile().hardware in the sketch.
// No board defaults; WakeNet is disabled unless explicitly enabled.
// Include one audio entry header in exactly one sketch translation unit.
#include "../boards/AudioProfile.h"

#if defined(XIAOZHI_AUDIO_PROFILE) && \
    XIAOZHI_AUDIO_PROFILE != XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
#error "I2sSimplexAudio.h conflicts with the selected audio profile"
#endif
#ifndef XIAOZHI_AUDIO_PROFILE
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
#endif

#include "I2sOpusAudioPort.impl.h"
