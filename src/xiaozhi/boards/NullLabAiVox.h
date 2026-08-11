#pragma once

// NULLLAB AI VOX (first generation, not AI-VOX3).
// Source: https://github.com/nulllaborg/ai_vox, example
// examples/ai_vox_board/ai_vox_board/main.cpp at commit 15587297fc94.

#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 1

// Direct-I2S speaker amplifier. The reference firmware uses I2S1, 32-bit
// Philips slots, and drives both slot masks from a mono PCM stream.
#define BOARD_AUDIO_OUTPUT_I2S_PORT 1
#define BOARD_AUDIO_OUTPUT_SAMPLE_RATE 24000
#define BOARD_AUDIO_OUTPUT_MCLK -1
#define BOARD_AUDIO_OUTPUT_BCLK 13
#define BOARD_AUDIO_OUTPUT_WS 14
#define BOARD_AUDIO_OUTPUT_DATA 1
#define BOARD_AUDIO_OUTPUT_DATA_BITS 32
#define BOARD_AUDIO_OUTPUT_VALID_BITS 16
#define BOARD_AUDIO_OUTPUT_SLOT_BITS 32
#define BOARD_AUDIO_OUTPUT_CHANNELS 1
#define BOARD_AUDIO_OUTPUT_SLOT I2sOpusAudioPort::I2sSlot::Both

// SPH0645 digital microphone on the independent I2S0 controller. The source
// board implementation shifts its 32-bit samples right by 14 before PCM16.
#define BOARD_AUDIO_INPUT_I2S_PORT 0
#define BOARD_AUDIO_INPUT_SAMPLE_RATE 16000
#define BOARD_AUDIO_INPUT_MCLK -1
#define BOARD_AUDIO_INPUT_BCLK 5
#define BOARD_AUDIO_INPUT_WS 2
#define BOARD_AUDIO_INPUT_DATA 4
#define BOARD_AUDIO_INPUT_DATA_BITS 32
#define BOARD_AUDIO_INPUT_VALID_BITS 24
#define BOARD_AUDIO_INPUT_SLOT_BITS 32
#define BOARD_AUDIO_INPUT_CHANNELS 1
#define BOARD_AUDIO_INPUT_SLOT I2sOpusAudioPort::I2sSlot::Left
#define BOARD_AUDIO_INPUT_RIGHT_SHIFT 14
#define BOARD_AUDIO_CAPTURE_CHANNEL I2sOpusAudioPort::CaptureChannel::Left

#define BOARD_AUDIO_AMP_ENABLE_PIN -1
#define BOARD_AUDIO_AMP_ACTIVE_LEVEL 1
#define BOARD_AUDIO_AMP_VOLUME_PERCENT 70
