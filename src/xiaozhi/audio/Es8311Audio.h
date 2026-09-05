#pragma once

// ES8311 audio with pins and gains set in Config::forCompiledProfile().hardware.
// No board defaults; WakeNet is disabled unless explicitly enabled.
// Include one audio entry header in exactly one sketch translation unit.
#include "../boards/AudioProfile.h"

#if defined(XIAOZHI_AUDIO_PROFILE) && \
    XIAOZHI_AUDIO_PROFILE != XIAOZHI_AUDIO_PROFILE_ES8311
#error "Es8311Audio.h conflicts with the selected audio profile"
#endif
#ifndef XIAOZHI_AUDIO_PROFILE
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_ES8311
#endif

#include "I2sOpusAudioPort.impl.h"
