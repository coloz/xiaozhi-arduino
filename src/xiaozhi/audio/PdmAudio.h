#pragma once

// PDM microphone + I2S speaker; fill Config::forCompiledProfile().hardware.
// No board defaults; WakeNet is disabled unless explicitly enabled.
// Include one audio entry header in exactly one sketch translation unit.
#include "../boards/AudioProfile.h"

#if defined(XIAOZHI_AUDIO_PROFILE) && \
    XIAOZHI_AUDIO_PROFILE != XIAOZHI_AUDIO_PROFILE_PDM_I2S
#error "PdmAudio.h conflicts with the selected audio profile"
#endif
#ifndef XIAOZHI_AUDIO_PROFILE
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_PDM_I2S
#endif

#include "I2sOpusAudioPort.impl.h"
