#pragma once

// Custom codec on shared I2S; fill Config::forCompiledProfile().hardware and codec.
// No board defaults; WakeNet is disabled unless explicitly enabled.
// Include one audio entry header in exactly one sketch translation unit.
#include "../boards/AudioProfile.h"

#if defined(XIAOZHI_AUDIO_PROFILE) && \
    XIAOZHI_AUDIO_PROFILE != XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC
#error "CustomCodecAudio.h conflicts with the selected audio profile"
#endif
#ifndef XIAOZHI_AUDIO_PROFILE
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC
#endif

#include "I2sOpusAudioPort.impl.h"
