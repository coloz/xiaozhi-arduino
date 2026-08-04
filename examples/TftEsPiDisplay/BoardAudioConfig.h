#pragma once

#include "BoardConfig.h"
#include "I2sOpusAudioPort.h"

#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
#include <Wire.h>
#endif

namespace xiaozhi_audio_board {

// Build the selected compile-time profile from board wiring macros. The
// returned object is ordinary runtime data: applications may still tune gains,
// queues, task sizes, slots, bit shifts, and wake behavior before construction.
inline I2sOpusAudioPort::Config makeConfig() {
  I2sOpusAudioPort::Config config =
      I2sOpusAudioPort::Config::forCompiledProfile();
  config.mclkMultiple = BOARD_AUDIO_MCLK_MULTIPLE;

  auto& output = config.hardware.output;
  output.port = BOARD_AUDIO_OUTPUT_I2S_PORT;
  output.sampleRate = BOARD_AUDIO_OUTPUT_SAMPLE_RATE;
  output.mclk = BOARD_AUDIO_OUTPUT_MCLK;
  output.bclk = BOARD_AUDIO_OUTPUT_BCLK;
  output.ws = BOARD_AUDIO_OUTPUT_WS;
  output.data = BOARD_AUDIO_OUTPUT_DATA;
  output.invertMclk = BOARD_AUDIO_OUTPUT_MCLK_INVERTED;
  output.invertBclk = BOARD_AUDIO_OUTPUT_BCLK_INVERTED;
  output.invertWs = BOARD_AUDIO_OUTPUT_WS_INVERTED;
  output.dataBits = BOARD_AUDIO_OUTPUT_DATA_BITS;
  output.validBits = BOARD_AUDIO_OUTPUT_VALID_BITS;
  output.slotBits = BOARD_AUDIO_OUTPUT_SLOT_BITS;
  output.channels = BOARD_AUDIO_OUTPUT_CHANNELS;
  output.slot = BOARD_AUDIO_OUTPUT_SLOT;

#if XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD
  auto& input = config.hardware.input;
  input.port = BOARD_AUDIO_INPUT_I2S_PORT;
  input.sampleRate = BOARD_AUDIO_INPUT_SAMPLE_RATE;
  input.mclk = BOARD_AUDIO_INPUT_MCLK;
  input.bclk = BOARD_AUDIO_INPUT_BCLK;
  input.ws = BOARD_AUDIO_INPUT_WS;
  input.data = BOARD_AUDIO_INPUT_DATA;
  input.invertMclk = BOARD_AUDIO_INPUT_MCLK_INVERTED;
  input.invertBclk = BOARD_AUDIO_INPUT_BCLK_INVERTED;
  input.invertWs = BOARD_AUDIO_INPUT_WS_INVERTED;
  input.dataBits = BOARD_AUDIO_INPUT_DATA_BITS;
  input.validBits = BOARD_AUDIO_INPUT_VALID_BITS;
  input.slotBits = BOARD_AUDIO_INPUT_SLOT_BITS;
  input.channels = BOARD_AUDIO_INPUT_CHANNELS;
  input.slot = BOARD_AUDIO_INPUT_SLOT;
  input.rightShift = BOARD_AUDIO_INPUT_RIGHT_SHIFT;
#endif

#if XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM
  auto& pdm = config.hardware.pdmInput;
  pdm.port = BOARD_AUDIO_PDM_I2S_PORT;
  pdm.sampleRate = BOARD_AUDIO_PDM_SAMPLE_RATE;
  pdm.clock = BOARD_AUDIO_PDM_CLOCK;
  pdm.data = BOARD_AUDIO_PDM_DATA;
  pdm.invertClock = BOARD_AUDIO_PDM_CLOCK_INVERTED;
#endif

  config.hardware.amplifier.enablePin = BOARD_AUDIO_AMP_ENABLE_PIN;
  config.hardware.amplifier.activeLevel = BOARD_AUDIO_AMP_ACTIVE_LEVEL;
  config.hardware.amplifier.volumePercent = BOARD_AUDIO_AMP_VOLUME_PERCENT;

#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
  auto& es8311 = config.hardware.es8311;
  es8311.wire = &Wire;
  es8311.initializeWire = BOARD_AUDIO_CODEC_INITIALIZE_WIRE;
  es8311.i2cSda = BOARD_AUDIO_I2C_SDA;
  es8311.i2cScl = BOARD_AUDIO_I2C_SCL;
  es8311.i2cFrequency = BOARD_AUDIO_I2C_FREQUENCY;
  es8311.address = BOARD_AUDIO_CODEC_ADDRESS;
  es8311.resetOnBegin = BOARD_AUDIO_CODEC_RESET_ON_BEGIN;
  es8311.resetDelayMs = BOARD_AUDIO_CODEC_RESET_DELAY_MS;
  es8311.paPin = BOARD_AUDIO_CODEC_PA_PIN;
  es8311.paActiveLevel = BOARD_AUDIO_CODEC_PA_ACTIVE_LEVEL;
  es8311.useMclk = BOARD_AUDIO_CODEC_USE_MCLK;
  es8311.master = BOARD_AUDIO_CODEC_MASTER;
  es8311.noDacReference = BOARD_AUDIO_CODEC_NO_DAC_REFERENCE;
  es8311.paSupplyVoltage = BOARD_AUDIO_PA_SUPPLY_VOLTAGE;
  es8311.codecDacVoltage = BOARD_AUDIO_CODEC_DAC_VOLTAGE;
  es8311.paGainDb = BOARD_AUDIO_PA_GAIN_DB;
  es8311.microphoneGainDb = BOARD_AUDIO_MIC_GAIN_DB;
  es8311.outputVolumeDb = BOARD_AUDIO_OUTPUT_VOLUME_DB;
#endif

  config.captureChannel = BOARD_AUDIO_CAPTURE_CHANNEL;
  return config;
}

inline bool probe(const I2sOpusAudioPort::Config& config) {
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
  const auto& es8311 = config.hardware.es8311;
  if (es8311.wire == nullptr) {
    return false;
  }
  if (es8311.initializeWire &&
      !es8311.wire->begin(es8311.i2cSda, es8311.i2cScl,
                          es8311.i2cFrequency)) {
    return false;
  }
  es8311.wire->beginTransmission(es8311.address);
  return es8311.wire->endTransmission() == 0;
#else
  (void)config;
  // Direct I2S/PDM modules expose no readable control register. Detailed pin,
  // rate, controller, and wire-format checks run in audioPort.begin().
  return true;
#endif
}

}  // namespace xiaozhi_audio_board
