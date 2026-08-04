#pragma once

#include "AudioProfile.h"

// Select one audio hardware implementation. This default keeps the original
// board working; alternatives and their required wiring are documented in
// README.md. The selection happens before I2sOpusAudioPort.impl.h is included,
// so unselected Codec/PDM/WakeNet code is removed by the preprocessor.
#ifndef XIAOZHI_AUDIO_PROFILE
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_ES8311
#endif
#ifndef XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR
#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 1
#endif

// Change this file to adapt the example to another ESP32-S3 board. The sample
// values describe one 240x240 ST7789 display plus an ES8311 audio codec; they
// are not tied to a board vendor.
#define BOARD_TYPE "esp32s3-tft-es8311"
#define BOARD_NAME "ESP32-S3 TFT voice terminal"

// Display wiring and behavior.
#define BOARD_LCD_WIDTH 240
#define BOARD_LCD_HEIGHT 240
#define BOARD_LCD_ROTATION 1
#define BOARD_LCD_SCLK 21
#define BOARD_LCD_MOSI 47
#define BOARD_LCD_MISO -1
#define BOARD_LCD_DC 43
#define BOARD_LCD_CS 44
#define BOARD_LCD_RST -1
#define BOARD_LCD_SPI_FREQUENCY 40000000
#define BOARD_LCD_SPI_READ_FREQUENCY 20000000

// Conversation button. It is active-low and uses the internal pull-up.
#define BOARD_CHAT_BUTTON 0
#define BOARD_CHAT_BUTTON_ACTIVE_LEVEL 0

// ES8311 control bus. Used only by XIAOZHI_AUDIO_PROFILE_ES8311.
#define BOARD_AUDIO_I2C_SDA 41
#define BOARD_AUDIO_I2C_SCL 42
#define BOARD_AUDIO_I2C_FREQUENCY 400000
#define BOARD_AUDIO_CODEC_ADDRESS 0x18

// Speaker standard-I2S endpoint. DATA is named from the ESP32 controller's
// point of view and therefore connects to the Codec/DAC/amp DIN pin.
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_PDM_I2S
#define BOARD_AUDIO_OUTPUT_I2S_PORT 1
#else
#define BOARD_AUDIO_OUTPUT_I2S_PORT 0
#endif
#define BOARD_AUDIO_OUTPUT_SAMPLE_RATE 24000
#define BOARD_AUDIO_OUTPUT_MCLK 46
#define BOARD_AUDIO_OUTPUT_BCLK 39
#define BOARD_AUDIO_OUTPUT_WS 2
#define BOARD_AUDIO_OUTPUT_DATA 38
#define BOARD_AUDIO_OUTPUT_MCLK_INVERTED false
#define BOARD_AUDIO_OUTPUT_BCLK_INVERTED false
#define BOARD_AUDIO_OUTPUT_WS_INVERTED false
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_ES8311 || \
    XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC
#define BOARD_AUDIO_OUTPUT_DATA_BITS 16
#define BOARD_AUDIO_OUTPUT_VALID_BITS 16
#define BOARD_AUDIO_OUTPUT_SLOT_BITS 16
#define BOARD_AUDIO_OUTPUT_CHANNELS 2
#define BOARD_AUDIO_OUTPUT_SLOT I2sOpusAudioPort::I2sSlot::Both
#else
#define BOARD_AUDIO_OUTPUT_DATA_BITS 32
#define BOARD_AUDIO_OUTPUT_VALID_BITS 16
#define BOARD_AUDIO_OUTPUT_SLOT_BITS 32
#define BOARD_AUDIO_OUTPUT_CHANNELS 1
#define BOARD_AUDIO_OUTPUT_SLOT I2sOpusAudioPort::I2sSlot::Left
#endif

// Standard-I2S microphone endpoint. ES8311 shares the output clocks. For an
// INMP441/MSM261 + MAX98357A simplex profile, change this to the second I2S
// controller and the microphone's independent SCK/WS/SD pins.
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
#define BOARD_AUDIO_INPUT_I2S_PORT 1
#define BOARD_AUDIO_INPUT_SAMPLE_RATE 16000
#else
#define BOARD_AUDIO_INPUT_I2S_PORT 0
#define BOARD_AUDIO_INPUT_SAMPLE_RATE 24000
#endif
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
// No board-independent pins are safe here: the second controller is also an
// I2S master. Fill all three required clock/data pins for the target board.
#define BOARD_AUDIO_INPUT_MCLK -1
#define BOARD_AUDIO_INPUT_BCLK -1
#define BOARD_AUDIO_INPUT_WS -1
#define BOARD_AUDIO_INPUT_DATA -1
#else
#define BOARD_AUDIO_INPUT_MCLK 46
#define BOARD_AUDIO_INPUT_BCLK 39
#define BOARD_AUDIO_INPUT_WS 2
#define BOARD_AUDIO_INPUT_DATA 40
#endif
#define BOARD_AUDIO_INPUT_MCLK_INVERTED false
#define BOARD_AUDIO_INPUT_BCLK_INVERTED false
#define BOARD_AUDIO_INPUT_WS_INVERTED false
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_ES8311 || \
    XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC
#define BOARD_AUDIO_INPUT_DATA_BITS 16
#define BOARD_AUDIO_INPUT_VALID_BITS 16
#define BOARD_AUDIO_INPUT_SLOT_BITS 16
#define BOARD_AUDIO_INPUT_CHANNELS 2
#define BOARD_AUDIO_INPUT_SLOT I2sOpusAudioPort::I2sSlot::Both
#define BOARD_AUDIO_INPUT_RIGHT_SHIFT 0
#else
// INMP441/MSM261 commonly deliver a 24-bit sample in a 32-bit slot. Adjust
// RIGHT_SHIFT after measuring raw peak/RMS; it is the fixed digital input gain.
#define BOARD_AUDIO_INPUT_DATA_BITS 32
#define BOARD_AUDIO_INPUT_VALID_BITS 24
#define BOARD_AUDIO_INPUT_SLOT_BITS 32
#define BOARD_AUDIO_INPUT_CHANNELS 1
#define BOARD_AUDIO_INPUT_SLOT I2sOpusAudioPort::I2sSlot::Left
#define BOARD_AUDIO_INPUT_RIGHT_SHIFT 12
#endif
#define BOARD_AUDIO_CAPTURE_CHANNEL I2sOpusAudioPort::CaptureChannel::Left

// PDM microphone endpoint. Used only by XIAOZHI_AUDIO_PROFILE_PDM_I2S.
#define BOARD_AUDIO_PDM_I2S_PORT 0
#define BOARD_AUDIO_PDM_SAMPLE_RATE 16000
#define BOARD_AUDIO_PDM_CLOCK -1
#define BOARD_AUDIO_PDM_DATA -1
#define BOARD_AUDIO_PDM_CLOCK_INVERTED false

// Optional MAX98357A SD/EN control. Leave -1 when it is tied active in
// hardware. Software volume is applied only to no-Codec 32-bit output.
#define BOARD_AUDIO_AMP_ENABLE_PIN -1
#define BOARD_AUDIO_AMP_ACTIVE_LEVEL true
#define BOARD_AUDIO_AMP_VOLUME_PERCENT 70

// ES8311 control and analog tuning. These macros are ignored by direct-I2S and
// PDM profiles. PA_PIN is the Codec driver's optional external-PA control pin.
#define BOARD_AUDIO_CODEC_INITIALIZE_WIRE true
#define BOARD_AUDIO_CODEC_RESET_ON_BEGIN true
#define BOARD_AUDIO_CODEC_RESET_DELAY_MS 10
#define BOARD_AUDIO_CODEC_PA_PIN -1
#define BOARD_AUDIO_CODEC_PA_ACTIVE_LEVEL true
#define BOARD_AUDIO_CODEC_USE_MCLK true
#define BOARD_AUDIO_CODEC_MASTER false
#define BOARD_AUDIO_CODEC_NO_DAC_REFERENCE true
#define BOARD_AUDIO_MCLK_MULTIPLE 256
#define BOARD_AUDIO_PA_SUPPLY_VOLTAGE 5.0f
#define BOARD_AUDIO_CODEC_DAC_VOLTAGE 3.3f
#define BOARD_AUDIO_PA_GAIN_DB 0.0f
#define BOARD_AUDIO_MIC_GAIN_DB 36.0f
#define BOARD_AUDIO_OUTPUT_VOLUME_DB -12.0f
