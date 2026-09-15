#pragma once

// OpenJumper OJoy ESP32-S3, ES8311 + CH13613 QSPI AMOLED.
// Wiring: OJoy pin table and aily-blockly-libraries/ojoy_es8311/generator.js.
// Display and other peripheral pins are configured by the application.

#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_ES8311
#ifndef XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR
#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 1
#endif

// Wire is shared with RTC, IMU and fuel gauge. Touch uses Wire1 separately.
#define BOARD_AUDIO_I2C_SDA 5
#define BOARD_AUDIO_I2C_SCL 4
#define BOARD_AUDIO_I2C_FREQUENCY 400000
#define BOARD_AUDIO_CODEC_ADDRESS 0x18

// DIN/DOUT in the board pin table are named from the codec's perspective.
// ESP32 TX -> ES8311 DIN (GPIO11); ES8311 DOUT -> ESP32 RX (GPIO13).
#define BOARD_AUDIO_OUTPUT_I2S_PORT 0
#define BOARD_AUDIO_OUTPUT_SAMPLE_RATE 24000
#define BOARD_AUDIO_OUTPUT_MCLK 6
#define BOARD_AUDIO_OUTPUT_BCLK 14
#define BOARD_AUDIO_OUTPUT_WS 12
#define BOARD_AUDIO_OUTPUT_DATA 11
#define BOARD_AUDIO_OUTPUT_DATA_BITS 16
#define BOARD_AUDIO_OUTPUT_VALID_BITS 16
#define BOARD_AUDIO_OUTPUT_SLOT_BITS 16
#define BOARD_AUDIO_OUTPUT_CHANNELS 2
#define BOARD_AUDIO_OUTPUT_SLOT I2sOpusAudioPort::I2sSlot::Both

#define BOARD_AUDIO_INPUT_I2S_PORT 0
#define BOARD_AUDIO_INPUT_SAMPLE_RATE 24000
#define BOARD_AUDIO_INPUT_MCLK 6
#define BOARD_AUDIO_INPUT_BCLK 14
#define BOARD_AUDIO_INPUT_WS 12
#define BOARD_AUDIO_INPUT_DATA 13
#define BOARD_AUDIO_INPUT_DATA_BITS 16
#define BOARD_AUDIO_INPUT_VALID_BITS 16
#define BOARD_AUDIO_INPUT_SLOT_BITS 16
#define BOARD_AUDIO_INPUT_CHANNELS 2
#define BOARD_AUDIO_INPUT_SLOT I2sOpusAudioPort::I2sSlot::Both
#define BOARD_AUDIO_INPUT_RIGHT_SHIFT 0
#define BOARD_AUDIO_CAPTURE_CHANNEL I2sOpusAudioPort::CaptureChannel::Left

// ES8311 owns PA control; BOARD_AUDIO_AMP_ENABLE_PIN is for codec-free profiles.
#define BOARD_AUDIO_CODEC_PA_PIN 9
#define BOARD_AUDIO_CODEC_PA_ACTIVE_LEVEL true
// One analog microphone through ES8311: left = mic, right = internal DAC
// reference for local wake AEC. Two I2S slots do not mean two microphones.
#define BOARD_AUDIO_CODEC_NO_DAC_REFERENCE false
// Keep near-end sensitivity while leaving extra speaker-to-mic headroom:
// the 24/-24 dB setup still clipped in a captured playback/interrupt test.
#ifndef BOARD_AUDIO_MIC_GAIN_DB
#define BOARD_AUDIO_MIC_GAIN_DB 24.0f
#endif
#ifndef BOARD_AUDIO_OUTPUT_VOLUME_DB
#define BOARD_AUDIO_OUTPUT_VOLUME_DB -30.0f
#endif
