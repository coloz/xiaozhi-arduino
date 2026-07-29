#include "I2sOpusAudioPort.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <model_path.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <EspressifEs8311.h>
#include <EspressifOpus.h>
#include <es8311_reg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <utility>
#include <vector>

namespace i2s_opus_detail {

esp_opus_enc_frame_duration_t encoderDuration(uint16_t durationMs) {
  switch (durationMs) {
    case 20:
      return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    case 40:
      return ESP_OPUS_ENC_FRAME_DURATION_40_MS;
    case 60:
      return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    default:
      return ESP_OPUS_ENC_FRAME_DURATION_ARG;
  }
}

esp_opus_dec_frame_duration_t decoderDuration(uint16_t durationMs) {
  switch (durationMs) {
    case 20:
      return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    case 40:
      return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
    case 60:
      return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    default:
      return ESP_OPUS_DEC_FRAME_DURATION_INVALID;
  }
}

struct WireCodecControl {
  audio_codec_ctrl_if_t base{};
  TwoWire* wire = nullptr;
  uint8_t address = 0x18;
  bool opened = false;
};

WireCodecControl* wireControl(const audio_codec_ctrl_if_t* control) {
  return reinterpret_cast<WireCodecControl*>(const_cast<audio_codec_ctrl_if_t*>(control));
}

int wireOpen(const audio_codec_ctrl_if_t* control, void*, int) {
  if (control == nullptr) {
    return ESP_CODEC_DEV_INVALID_ARG;
  }
  wireControl(control)->opened = true;
  return ESP_CODEC_DEV_OK;
}

bool wireIsOpen(const audio_codec_ctrl_if_t* control) {
  return control != nullptr && wireControl(control)->opened;
}

int wireRead(const audio_codec_ctrl_if_t* control, int reg, int regLength,
             void* data, int dataLength) {
  if (!wireIsOpen(control) || data == nullptr || regLength != 1 || dataLength <= 0) {
    return ESP_CODEC_DEV_INVALID_ARG;
  }
  WireCodecControl* state = wireControl(control);
  state->wire->beginTransmission(state->address);
  state->wire->write(static_cast<uint8_t>(reg));
  if (state->wire->endTransmission(false) != 0) {
    return ESP_CODEC_DEV_READ_FAIL;
  }
  const size_t received =
      state->wire->requestFrom(state->address, static_cast<uint8_t>(dataLength));
  if (received != static_cast<size_t>(dataLength)) {
    return ESP_CODEC_DEV_READ_FAIL;
  }
  uint8_t* output = static_cast<uint8_t*>(data);
  for (int index = 0; index < dataLength; ++index) {
    output[index] = static_cast<uint8_t>(state->wire->read());
  }
  return ESP_CODEC_DEV_OK;
}

int wireWrite(const audio_codec_ctrl_if_t* control, int reg, int regLength,
              void* data, int dataLength) {
  if (!wireIsOpen(control) || data == nullptr || regLength != 1 || dataLength < 0) {
    return ESP_CODEC_DEV_INVALID_ARG;
  }
  WireCodecControl* state = wireControl(control);
  state->wire->beginTransmission(state->address);
  state->wire->write(static_cast<uint8_t>(reg));
  state->wire->write(static_cast<const uint8_t*>(data), static_cast<size_t>(dataLength));
  return state->wire->endTransmission() == 0 ? ESP_CODEC_DEV_OK
                                             : ESP_CODEC_DEV_WRITE_FAIL;
}

int wireClose(const audio_codec_ctrl_if_t* control) {
  if (control == nullptr) {
    return ESP_CODEC_DEV_INVALID_ARG;
  }
  wireControl(control)->opened = false;
  return ESP_CODEC_DEV_OK;
}

}  // namespace i2s_opus_detail

struct I2sOpusAudioPort::Impl {
  struct PlaybackChunk {
    std::vector<int16_t> pcm;
    uint32_t sourceRate = 0;
    uint16_t durationMs = 0;
    uint8_t sourceChannels = 0;
  };

  explicit Impl(const Config& value) : config(value) {}

  Config config;
  xiaozhi::AudioFormat captureFormat{};
  Uplink uplink;
  bool started = false;
  bool captureEnabled = false;

  i2s_chan_handle_t tx = nullptr;
  i2s_chan_handle_t rx = nullptr;
  SemaphoreHandle_t rxMutex = nullptr;
  i2s_opus_detail::WireCodecControl control{};
  const audio_codec_if_t* codec = nullptr;
  bool externalCodecStarted = false;
  void* encoder = nullptr;
  void* decoder = nullptr;
  uint32_t decoderRate = 0;
  uint16_t decoderDurationMs = 0;

  int encoderInputBytes = 0;
  int encoderOutputBytes = 0;
  std::vector<uint8_t> encoded;
  size_t hardwareFramesPerPacket = 0;
  size_t captureSamplesPerPacket = 0;
  size_t i2sChannels = 0;
  size_t maximumDecodePackets = 0;
  std::vector<int16_t> captured;
  std::vector<int16_t> captureMono;
  std::vector<int16_t> readBuffer;
  size_t capturedSamples = 0;
  std::deque<xiaozhi::AudioFrame> decodeQueue;
  std::deque<PlaybackChunk> playbackQueue;
  SemaphoreHandle_t queueMutex = nullptr;
  TaskHandle_t decoderTask = nullptr;
  TaskHandle_t outputTask = nullptr;
  bool workersRunning = false;
  bool decodeInFlight = false;
  bool outputInFlight = false;
  uint32_t lastPlaybackWriteMs = 0;
  uint32_t capturedPackets = 0;
  uint32_t playedPackets = 0;
  uint32_t droppedDecodePackets = 0;
  bool playbackMuted = true;
  bool wakeDetectionStarted = false;
  std::atomic<bool> wakeDetectionEnabled{false};
  std::atomic<bool> wakeDetected{false};
  std::atomic<bool> wakeTaskRunning{false};
  TaskHandle_t wakeTask = nullptr;
  srmodel_list_t* wakeModels = nullptr;
  const esp_wn_iface_t* wakeNet = nullptr;
  model_iface_data_t* wakeNetData = nullptr;
  size_t wakeChunkSamples = 0;
  char lastWakeWord[96]{};
  std::vector<int16_t> wakeCaptured;
  std::vector<int16_t> wakeMono;
  std::vector<int16_t> wakeDetectChunk;
  std::deque<int16_t> wakePcm;

  bool beginI2s() {
    rxMutex = xSemaphoreCreateMutex();
    if (rxMutex == nullptr) {
      Serial.println("[audio] failed to create I2S RX mutex");
      return false;
    }
    i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(
        static_cast<i2s_port_t>(config.i2sPort), I2S_ROLE_MASTER);
    channelConfig.dma_desc_num = config.dmaDescriptorCount;
    channelConfig.dma_frame_num = config.dmaFrames;
    channelConfig.auto_clear_after_cb = true;
    channelConfig.auto_clear_before_cb = false;
    if (i2s_new_channel(&channelConfig, &tx, &rx) != ESP_OK) {
      Serial.println("[audio] failed to allocate I2S channels");
      return false;
    }

    i2s_std_config_t standard{};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config.hardwareSampleRate);
    standard.clk_cfg.mclk_multiple =
        static_cast<i2s_mclk_multiple_t>(config.mclkMultiple);
    standard.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        config.stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
    standard.slot_cfg.slot_mask = config.stereo ? I2S_STD_SLOT_BOTH : I2S_STD_SLOT_LEFT;
    standard.gpio_cfg.mclk = static_cast<gpio_num_t>(config.mclk);
    standard.gpio_cfg.bclk = static_cast<gpio_num_t>(config.bclk);
    standard.gpio_cfg.ws = static_cast<gpio_num_t>(config.ws);
    standard.gpio_cfg.dout = static_cast<gpio_num_t>(config.dataOut);
    standard.gpio_cfg.din = static_cast<gpio_num_t>(config.dataIn);
    standard.gpio_cfg.invert_flags.mclk_inv = config.invertMclk;
    standard.gpio_cfg.invert_flags.bclk_inv = config.invertBclk;
    standard.gpio_cfg.invert_flags.ws_inv = config.invertWs;

    if (i2s_channel_init_std_mode(tx, &standard) != ESP_OK ||
        i2s_channel_init_std_mode(rx, &standard) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
      Serial.println("[audio] failed to initialize I2S standard mode");
      return false;
    }
    return true;
  }

  bool setCodecMuted(bool muted) {
    if (externalCodecStarted) {
      return config.codec.setMuted(config.codec.context, muted);
    }
    return codec != nullptr && codec->mute(codec, muted) == ESP_CODEC_DEV_OK;
  }

  bool beginCodec() {
    const CodecFormat format{config.hardwareSampleRate, 16,
                             static_cast<uint8_t>(i2sChannels),
                             config.mclkMultiple};
    if (config.codec.begin != nullptr) {
      externalCodecStarted = config.codec.begin(config.codec.context, format);
      if (!externalCodecStarted) {
        Serial.println("[audio] external codec initialization failed");
        return false;
      }
      if (!setCodecMuted(true)) {
        Serial.println("[audio] external codec initial mute failed");
        return false;
      }
      playbackMuted = true;
      return true;
    }

    if (config.initializeWire &&
        !config.wire->begin(config.i2cSda, config.i2cScl, config.i2cFrequency)) {
      Serial.println("[audio] failed to initialize codec I2C bus");
      return false;
    }

    // A processor-only warm restart may leave the codec active or partially
    // configured, so the built-in driver can replay its reset sequence.
    const std::pair<uint8_t, uint8_t> resetSequence[] = {
        {ES8311_DAC_REG32, 0x00},         {ES8311_ADC_REG17, 0x00},
        {ES8311_SYSTEM_REG0E, 0xFF},      {ES8311_SYSTEM_REG12, 0x02},
        {ES8311_SYSTEM_REG14, 0x00},      {ES8311_SYSTEM_REG0D, 0xFA},
        {ES8311_ADC_REG15, 0x00},         {ES8311_CLK_MANAGER_REG02, 0x10},
        {ES8311_RESET_REG00, 0x00},       {ES8311_RESET_REG00, 0x1F},
        {ES8311_CLK_MANAGER_REG01, 0x30}, {ES8311_CLK_MANAGER_REG01, 0x00},
        {ES8311_GP_REG45, 0x00},          {ES8311_SYSTEM_REG0D, 0xFC},
        {ES8311_CLK_MANAGER_REG02, 0x00},
    };
    if (config.resetCodecOnBegin) {
      for (const auto& item : resetSequence) {
        config.wire->beginTransmission(config.codecAddress);
        config.wire->write(item.first);
        config.wire->write(item.second);
        if (config.wire->endTransmission() != 0) {
          Serial.printf("[audio] codec reset write failed at register 0x%02X\n",
                        item.first);
          return false;
        }
      }
      delay(config.codecResetDelayMs);
    }

    control.base.open = i2s_opus_detail::wireOpen;
    control.base.is_open = i2s_opus_detail::wireIsOpen;
    control.base.read_reg = i2s_opus_detail::wireRead;
    control.base.write_reg = i2s_opus_detail::wireWrite;
    control.base.close = i2s_opus_detail::wireClose;
    control.wire = config.wire;
    control.address = config.codecAddress;
    control.opened = true;

    es8311_codec_cfg_t codecConfig{};
    codecConfig.ctrl_if = &control.base;
    codecConfig.gpio_if = nullptr;
    codecConfig.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    codecConfig.pa_pin = config.codecPaPin;
    codecConfig.use_mclk = config.codecUseMclk;
    codecConfig.master_mode = config.codecMaster;
    codecConfig.no_dac_ref = config.codecNoDacReference;
    codecConfig.mclk_div = config.mclkMultiple;
    codecConfig.hw_gain.pa_voltage = config.paSupplyVoltage;
    codecConfig.hw_gain.codec_dac_voltage = config.codecDacVoltage;
    codecConfig.hw_gain.pa_gain = config.paGainDb;
    codec = es8311_codec_new(&codecConfig);
    if (codec == nullptr) {
      Serial.println("[audio] built-in codec initialization failed");
      return false;
    }

    esp_codec_dev_sample_info_t sampleInfo{};
    sampleInfo.bits_per_sample = 16;
    sampleInfo.channel = static_cast<uint8_t>(i2sChannels);
    sampleInfo.channel_mask = config.stereo ? 0x03 : 0x01;
    sampleInfo.sample_rate = config.hardwareSampleRate;
    sampleInfo.mclk_multiple = config.mclkMultiple;
    const int fsResult = codec->set_fs(codec, &sampleInfo);
    const int gainResult = codec->set_mic_gain(codec, config.microphoneGainDb);
    const int volumeResult = codec->set_vol(codec, config.outputVolumeDb);
    const int enableResult = codec->enable(codec, true);
    if (fsResult != ESP_CODEC_DEV_OK || gainResult != ESP_CODEC_DEV_OK ||
        volumeResult != ESP_CODEC_DEV_OK || enableResult != ESP_CODEC_DEV_OK) {
      Serial.printf("[audio] codec setup failed: fs=%d gain=%d volume=%d enable=%d\n",
                    fsResult, gainResult, volumeResult, enableResult);
      return false;
    }
    // BOTH enables ADC and DAC at the same time.  Keep the DAC muted until a
    // decoded speech packet arrives so an idle TX FIFO cannot become hiss.
    if (!setCodecMuted(true)) {
      Serial.println("[audio] codec initial mute failed");
      return false;
    }
    playbackMuted = true;
    return true;
  }

  bool beginEncoder() {
    esp_opus_enc_config_t encoderConfig{};
    encoderConfig.sample_rate = captureFormat.sample_rate;
    encoderConfig.channel = ESP_AUDIO_MONO;
    encoderConfig.bits_per_sample = ESP_AUDIO_BIT16;
    encoderConfig.bitrate = config.opusBitrate;
    encoderConfig.frame_duration =
        i2s_opus_detail::encoderDuration(captureFormat.frame_duration_ms);
    encoderConfig.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    encoderConfig.complexity = config.opusComplexity;
    encoderConfig.enable_fec = config.opusFec;
    encoderConfig.enable_dtx = config.opusDtx;
    encoderConfig.enable_vbr = config.opusVbr;
    if (encoderConfig.frame_duration == ESP_OPUS_ENC_FRAME_DURATION_ARG ||
        esp_opus_enc_open(&encoderConfig, sizeof(encoderConfig), &encoder) !=
            ESP_AUDIO_ERR_OK ||
        encoder == nullptr ||
        esp_opus_enc_get_frame_size(encoder, &encoderInputBytes, &encoderOutputBytes) !=
            ESP_AUDIO_ERR_OK) {
      Serial.println("[audio] Opus encoder initialization failed");
      return false;
    }
    if (encoderInputBytes != static_cast<int>(captureSamplesPerPacket * sizeof(int16_t)) ||
        encoderOutputBytes <= 0) {
      Serial.printf("[audio] unexpected Opus frame sizes: input=%d output=%d\n",
                    encoderInputBytes, encoderOutputBytes);
      return false;
    }
    encoded.resize(static_cast<size_t>(encoderOutputBytes));
    return true;
  }

  static void wakeTaskEntry(void* context) {
    static_cast<Impl*>(context)->wakeLoop();
  }

  void wakeLoop() {
    while (wakeTaskRunning.load()) {
      if (!wakeDetectionEnabled.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        continue;
      }

      size_t inputBytes = 0;
      esp_err_t result = ESP_ERR_TIMEOUT;
      if (rxMutex != nullptr && xSemaphoreTake(rxMutex, portMAX_DELAY) == pdTRUE) {
        if (wakeDetectionEnabled.load()) {
          result = i2s_channel_read(rx, wakeCaptured.data(),
                                    wakeCaptured.size() * sizeof(int16_t),
                                    &inputBytes, portMAX_DELAY);
        }
        xSemaphoreGive(rxMutex);
      }
      if (!wakeTaskRunning.load() || !wakeDetectionEnabled.load()) {
        continue;
      }
      if (result != ESP_OK || inputBytes != wakeCaptured.size() * sizeof(int16_t)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }

      char channel = 'L';
      uint32_t peak = 0;
      uint32_t rms = 0;
      resampleCapture(wakeCaptured.data(), wakeMono, channel, peak, rms);
      wakePcm.insert(wakePcm.end(), wakeMono.begin(), wakeMono.end());

      while (wakeDetectionEnabled.load() && wakePcm.size() >= wakeChunkSamples) {
        for (size_t index = 0; index < wakeChunkSamples; ++index) {
          wakeDetectChunk[index] = wakePcm.front();
          wakePcm.pop_front();
        }
        const int detected = wakeNet->detect(wakeNetData, wakeDetectChunk.data());
        if (detected <= 0) {
          continue;
        }
        const char* word = wakeNet->get_word_name(wakeNetData, detected);
        snprintf(lastWakeWord, sizeof(lastWakeWord), "%s",
                 word == nullptr || word[0] == '\0' ? config.defaultWakeWord : word);
        wakeDetectionEnabled.store(false);
        wakePcm.clear();
        // Publish the event only after lastWakeWord has been completely written.
        wakeDetected.store(true);
        break;
      }
    }
    wakeTask = nullptr;
    vTaskDelete(nullptr);
  }

  bool beginWakeDetection() {
    if (!config.enableWakeDetection) {
      return true;
    }
    wakeDetectionEnabled.store(false);
    wakeDetected.store(false);
    wakeModels = esp_srmodel_init(config.wakeModelPartition);
    if (wakeModels == nullptr || wakeModels->num <= 0) {
      Serial.println("[wake] no WakeNet model found in the model partition");
      return false;
    }
    char* modelName = esp_srmodel_filter(wakeModels, ESP_WN_PREFIX, nullptr);
    if (modelName == nullptr ||
        (wakeNet = static_cast<const esp_wn_iface_t*>(
             esp_wn_handle_from_name(modelName))) == nullptr ||
        (wakeNetData = wakeNet->create(
             modelName, config.wakeDetectionMode == WakeDetectionMode::Aggressive
                            ? DET_MODE_95
                            : DET_MODE_90)) == nullptr) {
      Serial.println("[wake] WakeNet model initialization failed");
      return false;
    }
    const int sampleRate = wakeNet->get_samp_rate(wakeNetData);
    const int chunkSamples = wakeNet->get_samp_chunksize(wakeNetData);
    if (sampleRate != static_cast<int>(captureFormat.sample_rate) || chunkSamples <= 0) {
      Serial.printf("[wake] unsupported model format: rate=%d chunk=%d\n",
                    sampleRate, chunkSamples);
      return false;
    }
    wakeChunkSamples = static_cast<size_t>(chunkSamples);
    wakeDetectChunk.resize(wakeChunkSamples);
    wakeTaskRunning.store(true);
    if (xTaskCreatePinnedToCore(wakeTaskEntry, "wakenet", config.wakeTaskStackBytes,
                                this, config.wakeTaskPriority, &wakeTask,
                                config.wakeTaskCore) != pdPASS) {
      wakeTaskRunning.store(false);
      Serial.println("[wake] failed to create WakeNet task");
      return false;
    }
    wakeDetectionStarted = true;
    Serial.printf("[wake] WakeNet ready: model=%s rate=%d chunk=%d\n",
                  modelName, sampleRate, chunkSamples);
    return true;
  }

  void setWakeDetection(bool enabled) {
    if (!wakeDetectionStarted || wakeDetectionEnabled.load() == enabled) {
      return;
    }
    if (enabled) {
      if (rxMutex != nullptr && xSemaphoreTake(rxMutex, portMAX_DELAY) == pdTRUE) {
        wakePcm.clear();
        xSemaphoreGive(rxMutex);
      }
      wakeDetected.store(false);
      wakeDetectionEnabled.store(true);
      if (wakeTask != nullptr) {
        xTaskNotifyGive(wakeTask);
      }
      Serial.printf("[wake] listening for: %s\n", config.defaultWakeWord);
      return;
    }
    wakeDetectionEnabled.store(false);
    Serial.println("[wake] paused");
  }

  bool consumeWake(std::string& wakeWord) {
    if (!wakeDetected.exchange(false)) {
      return false;
    }
    setWakeDetection(false);
    wakeWord = lastWakeWord[0] == '\0' ? config.defaultWakeWord : lastWakeWord;
    return true;
  }

  void stopWakeDetection() {
    if (!wakeDetectionStarted && wakeTask == nullptr && wakeNetData == nullptr &&
        wakeModels == nullptr) {
      return;
    }
    wakeDetectionEnabled.store(false);
    wakeDetected.store(false);
    wakeTaskRunning.store(false);
    if (wakeTask != nullptr) {
      xTaskNotifyGive(wakeTask);
    }
    for (int attempt = 0; attempt < 100 && wakeTask != nullptr; ++attempt) {
      delay(10);
    }
    if (wakeTask != nullptr) {
      vTaskDelete(wakeTask);
      wakeTask = nullptr;
    }
    if (wakeNetData != nullptr) {
      wakeNet->destroy(wakeNetData);
      wakeNetData = nullptr;
    }
    wakeNet = nullptr;
    if (wakeModels != nullptr) {
      esp_srmodel_deinit(wakeModels);
      wakeModels = nullptr;
    }
    wakeDetectChunk.clear();
    wakePcm.clear();
    wakeChunkSamples = 0;
    wakeDetectionStarted = false;
  }

  bool ensureDecoder(uint32_t rate, uint16_t durationMs) {
    if (decoder != nullptr && decoderRate == rate && decoderDurationMs == durationMs) {
      return true;
    }
    if (decoder != nullptr) {
      esp_opus_dec_close(decoder);
      decoder = nullptr;
    }
    esp_opus_dec_cfg_t config{};
    config.sample_rate = rate;
    config.channel = ESP_AUDIO_MONO;
    config.frame_duration = i2s_opus_detail::decoderDuration(durationMs);
    config.self_delimited = false;
    const esp_audio_err_t result = esp_opus_dec_open(&config, sizeof(config), &decoder);
    if (result != ESP_AUDIO_ERR_OK || decoder == nullptr) {
      Serial.printf("[audio] Opus decoder initialization failed: %d\n", result);
      return false;
    }
    decoderRate = rate;
    decoderDurationMs = durationMs;
    return true;
  }

  static void decoderTaskEntry(void* context) {
    static_cast<Impl*>(context)->decoderLoop();
  }

  static void outputTaskEntry(void* context) {
    static_cast<Impl*>(context)->outputLoop();
  }

  bool startWorkers() {
    queueMutex = xSemaphoreCreateMutex();
    if (queueMutex == nullptr) {
      Serial.println("[audio] failed to create audio queue mutex");
      return false;
    }
    workersRunning = true;
    if (xTaskCreate(outputTaskEntry, "audio_output", config.outputTaskStackBytes,
                    this, config.outputTaskPriority, &outputTask) != pdPASS ||
        xTaskCreate(decoderTaskEntry, "opus_decoder", config.decoderTaskStackBytes,
                    this, config.decoderTaskPriority, &decoderTask) != pdPASS) {
      Serial.println("[audio] failed to create decoder/output tasks");
      stopWorkers();
      return false;
    }
    return true;
  }

  void stopWorkers() {
    if (queueMutex == nullptr) {
      return;
    }
    workersRunning = false;
    if (decoderTask != nullptr) {
      xTaskNotifyGive(decoderTask);
    }
    if (outputTask != nullptr) {
      xTaskNotifyGive(outputTask);
    }
    for (int attempt = 0;
         attempt < 100 && (decoderTask != nullptr || outputTask != nullptr); ++attempt) {
      delay(10);
    }
    if (decoderTask != nullptr) {
      vTaskDelete(decoderTask);
      decoderTask = nullptr;
    }
    if (outputTask != nullptr) {
      vTaskDelete(outputTask);
      outputTask = nullptr;
    }
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      decodeQueue.clear();
      playbackQueue.clear();
      decodeInFlight = false;
      outputInFlight = false;
      xSemaphoreGive(queueMutex);
    }
    vSemaphoreDelete(queueMutex);
    queueMutex = nullptr;
  }

  void decoderLoop() {
    while (workersRunning) {
      xiaozhi::AudioFrame frame;
      bool haveFrame = false;
      if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        if (workersRunning && !decodeQueue.empty() &&
            playbackQueue.size() < config.maximumPlaybackChunks) {
          frame = std::move(decodeQueue.front());
          decodeQueue.pop_front();
          decodeInFlight = true;
          haveFrame = true;
        }
        xSemaphoreGive(queueMutex);
      }
      if (!workersRunning) {
        break;
      }
      if (!haveFrame) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        continue;
      }

      PlaybackChunk chunk;
      const bool decoded = decodeFrame(frame, chunk);
      if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        if (decoded && workersRunning) {
          playbackQueue.push_back(std::move(chunk));
        }
        decodeInFlight = false;
        xSemaphoreGive(queueMutex);
      }
      if (outputTask != nullptr) {
        xTaskNotifyGive(outputTask);
      }
    }
    decoderTask = nullptr;
    vTaskDelete(nullptr);
  }

  void outputLoop() {
    while (workersRunning) {
      PlaybackChunk chunk;
      bool haveChunk = false;
      if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        if (workersRunning && !playbackQueue.empty()) {
          chunk = std::move(playbackQueue.front());
          playbackQueue.pop_front();
          outputInFlight = true;
          haveChunk = true;
        }
        xSemaphoreGive(queueMutex);
      }
      if (!workersRunning) {
        break;
      }
      if (haveChunk) {
        writePlaybackChunk(chunk);
        if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
          outputInFlight = false;
          xSemaphoreGive(queueMutex);
        }
        if (decoderTask != nullptr) {
          xTaskNotifyGive(decoderTask);
        }
        continue;
      }

      bool drained = false;
      if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        drained = decodeQueue.empty() && playbackQueue.empty() &&
                  !decodeInFlight && !outputInFlight;
        xSemaphoreGive(queueMutex);
      }
      if (drained && !playbackMuted &&
          millis() - lastPlaybackWriteMs > config.playbackMuteDelayMs) {
        if (setCodecMuted(true)) {
          playbackMuted = true;
        }
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
    outputTask = nullptr;
    vTaskDelete(nullptr);
  }

  void stop() {
    captureEnabled = false;
    stopWakeDetection();
    stopWorkers();
    uplink = {};
    if (encoder != nullptr) {
      esp_opus_enc_close(encoder);
      encoder = nullptr;
    }
    if (decoder != nullptr) {
      esp_opus_dec_close(decoder);
      decoder = nullptr;
    }
    if (externalCodecStarted) {
      config.codec.end(config.codec.context);
      externalCodecStarted = false;
    } else if (codec != nullptr) {
      codec->enable(codec, false);
      codec->close(codec);
      free(const_cast<audio_codec_if_t*>(codec));
      codec = nullptr;
    }
    if (tx != nullptr) {
      i2s_channel_disable(tx);
    }
    if (rx != nullptr) {
      i2s_channel_disable(rx);
    }
    if (tx != nullptr) {
      i2s_del_channel(tx);
      tx = nullptr;
    }
    if (rx != nullptr) {
      i2s_del_channel(rx);
      rx = nullptr;
    }
    if (rxMutex != nullptr) {
      vSemaphoreDelete(rxMutex);
      rxMutex = nullptr;
    }
    started = false;
  }

  void resampleCapture(const int16_t* interleaved, std::vector<int16_t>& output,
                       char& selectedChannel, uint32_t& peak, uint32_t& rms) {
    uint64_t leftEnergy = 0;
    uint64_t rightEnergy = 0;
    for (size_t frame = 0; frame < hardwareFramesPerPacket; ++frame) {
      const int32_t left = interleaved[frame * i2sChannels];
      const int32_t right = i2sChannels == 2
                                ? interleaved[frame * i2sChannels + 1]
                                : left;
      leftEnergy += static_cast<uint64_t>(left * left);
      rightEnergy += static_cast<uint64_t>(right * right);
    }
    size_t channel = 0;
    if (i2sChannels == 2 &&
        (config.captureChannel == CaptureChannel::Right ||
         (config.captureChannel == CaptureChannel::Auto &&
          static_cast<double>(rightEnergy) >
              static_cast<double>(leftEnergy) * config.autoChannelSwitchRatio))) {
      channel = 1;
    }
    selectedChannel = channel == 0 ? 'L' : 'R';
    uint64_t outputEnergy = 0;
    peak = 0;
    for (size_t index = 0; index < output.size(); ++index) {
      const double position = output.size() > 1
                                  ? static_cast<double>(index) *
                                        (hardwareFramesPerPacket - 1) /
                                        static_cast<double>(output.size() - 1)
                                  : 0.0;
      const size_t first = static_cast<size_t>(position);
      const size_t second = std::min(first + 1, hardwareFramesPerPacket - 1);
      const double fraction = position - static_cast<double>(first);
      const int32_t firstSample = interleaved[first * i2sChannels + channel];
      const int32_t secondSample = interleaved[second * i2sChannels + channel];
      const int32_t sample = firstSample + static_cast<int32_t>(
                                              (secondSample - firstSample) * fraction);
      output[index] = static_cast<int16_t>(sample);
      const uint32_t magnitude = static_cast<uint32_t>(std::abs(sample));
      peak = std::max(peak, magnitude);
      outputEnergy += static_cast<uint64_t>(sample * sample);
    }
    rms = static_cast<uint32_t>(std::sqrt(
        static_cast<double>(outputEnergy) / static_cast<double>(output.size())));
  }

  void encodeCapturedPacket() {
    char channel = 'L';
    uint32_t peak = 0;
    uint32_t rms = 0;
    resampleCapture(captured.data(), captureMono, channel, peak, rms);

    esp_audio_enc_in_frame_t input{};
    input.buffer = reinterpret_cast<uint8_t*>(captureMono.data());
    input.len = static_cast<uint32_t>(captureMono.size() * sizeof(int16_t));
    esp_audio_enc_out_frame_t output{};
    output.buffer = encoded.data();
    output.len = static_cast<uint32_t>(encoded.size());
    const esp_audio_err_t result = esp_opus_enc_process(encoder, &input, &output);
    if (result != ESP_AUDIO_ERR_OK || output.encoded_bytes == 0) {
      Serial.printf("[audio] Opus encode failed: %d\n", result);
      return;
    }
    ++capturedPackets;
    if (!uplink(encoded.data(), output.encoded_bytes, millis())) {
      Serial.println("[audio] uplink rejected an encoded frame");
    }
    if (capturedPackets == 1 || capturedPackets % 50 == 0) {
      Serial.printf("[audio] capture packet=%lu channel=%c peak=%lu rms=%lu opus=%lu\n",
                    static_cast<unsigned long>(capturedPackets), channel,
                    static_cast<unsigned long>(peak), static_cast<unsigned long>(rms),
                    static_cast<unsigned long>(output.encoded_bytes));
    }
  }

  void pumpCapture() {
    if (!captureEnabled || rxMutex == nullptr ||
        xSemaphoreTake(rxMutex, 0) != pdTRUE) {
      capturedSamples = 0;
      return;
    }
    size_t bytesRead = 0;
    const esp_err_t result =
        i2s_channel_read(rx, readBuffer.data(), readBuffer.size() * sizeof(int16_t),
                         &bytesRead, 0);
    xSemaphoreGive(rxMutex);
    if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
      Serial.printf("[audio] I2S read failed: %d\n", result);
      return;
    }
    const size_t samplesRead = bytesRead / sizeof(int16_t);
    size_t sourceOffset = 0;
    while (sourceOffset < samplesRead) {
      const size_t available = captured.size() - capturedSamples;
      const size_t copying = std::min(available, samplesRead - sourceOffset);
      std::copy_n(readBuffer.data() + sourceOffset, copying,
                  captured.data() + capturedSamples);
      capturedSamples += copying;
      sourceOffset += copying;
      if (capturedSamples == captured.size()) {
        encodeCapturedPacket();
        capturedSamples = 0;
      }
    }
  }

  bool decodeFrame(const xiaozhi::AudioFrame& frame, PlaybackChunk& chunk) {
    const uint32_t sourceRate = frame.format.sample_rate;
    const uint16_t durationMs = frame.format.frame_duration_ms;
    if (sourceRate == 0 || durationMs == 0 || frame.opus.empty() ||
        !ensureDecoder(sourceRate, durationMs)) {
      return false;
    }

    const size_t maximumFrames = sourceRate * durationMs / 1000 + 16;
    std::vector<int16_t> decoded(maximumFrames * 2);
    esp_audio_dec_in_raw_t input{};
    input.buffer = const_cast<uint8_t*>(frame.opus.data());
    input.len = static_cast<uint32_t>(frame.opus.size());
    esp_audio_dec_out_frame_t output{};
    output.buffer = reinterpret_cast<uint8_t*>(decoded.data());
    output.len = static_cast<uint32_t>(decoded.size() * sizeof(int16_t));
    esp_audio_dec_info_t info{};
    const esp_audio_err_t result = esp_opus_dec_decode(decoder, &input, &output, &info);
    if (result != ESP_AUDIO_ERR_OK || output.decoded_size == 0 || info.channel == 0) {
      Serial.printf("[audio] Opus decode failed: %d decoded=%lu\n", result,
                    static_cast<unsigned long>(output.decoded_size));
      return false;
    }

    const size_t decodedSamples = output.decoded_size / sizeof(int16_t);
    const size_t sourceFrames = decodedSamples / info.channel;
    if (sourceFrames == 0) {
      return false;
    }
    if (info.sample_rate == 0) {
      Serial.println("[audio] Opus decoder returned a zero sample rate");
      return false;
    }
    const size_t destinationFrames = static_cast<size_t>(
        (static_cast<uint64_t>(sourceFrames) * config.hardwareSampleRate +
         info.sample_rate / 2) /
        info.sample_rate);
    chunk.pcm.resize(destinationFrames * i2sChannels);
    chunk.sourceRate = info.sample_rate;
    chunk.durationMs = durationMs;
    chunk.sourceChannels = info.channel;
    for (size_t index = 0; index < destinationFrames; ++index) {
      const double position = destinationFrames > 1
                                  ? static_cast<double>(index) * (sourceFrames - 1) /
                                        static_cast<double>(destinationFrames - 1)
                                  : 0.0;
      const size_t first = static_cast<size_t>(position);
      const size_t second = std::min(first + 1, sourceFrames - 1);
      const double fraction = position - static_cast<double>(first);
      const int32_t firstSample = decoded[first * info.channel];
      const int32_t secondSample = decoded[second * info.channel];
      const int16_t sample = static_cast<int16_t>(
          firstSample + static_cast<int32_t>((secondSample - firstSample) * fraction));
      for (size_t channel = 0; channel < i2sChannels; ++channel) {
        chunk.pcm[index * i2sChannels + channel] = sample;
      }
    }
    return true;
  }

  void writePlaybackChunk(const PlaybackChunk& chunk) {
    if (playbackMuted) {
      if (!setCodecMuted(false)) {
        Serial.println("[audio] failed to unmute codec");
        return;
      }
      playbackMuted = false;
    }

    // This runs only on the dedicated audio_output task, matching the original
    // xiaozhi-esp32 AudioService. The network task never blocks on I2S.
    const uint8_t* pcm = reinterpret_cast<const uint8_t*>(chunk.pcm.data());
    const size_t totalBytes = chunk.pcm.size() * sizeof(int16_t);
    size_t offset = 0;
    while (offset < totalBytes) {
      size_t written = 0;
      const esp_err_t writeResult =
          i2s_channel_write(tx, pcm + offset, totalBytes - offset, &written,
                            pdMS_TO_TICKS(config.playbackWriteTimeoutMs));
      if (writeResult != ESP_OK || written == 0) {
        Serial.printf("[audio] I2S streaming write failed: result=%d offset=%lu/%lu\n",
                      writeResult, static_cast<unsigned long>(offset),
                      static_cast<unsigned long>(totalBytes));
        if (setCodecMuted(true)) {
          playbackMuted = true;
        }
        return;
      }
      offset += written;
      lastPlaybackWriteMs = millis();
    }
    ++playedPackets;
    if (playedPackets == 1 || playedPackets % 50 == 0) {
      Serial.printf("[audio] playback packet=%lu: %lu Hz, %u channel(s), %u ms\n",
                    static_cast<unsigned long>(playedPackets),
                    static_cast<unsigned long>(chunk.sourceRate), chunk.sourceChannels,
                    chunk.durationMs);
    }
  }

  void enqueueDecode(const xiaozhi::AudioFrame& frame) {
    if (!workersRunning || queueMutex == nullptr) {
      return;
    }
    bool queued = false;
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
      if (decodeQueue.size() < maximumDecodePackets) {
        decodeQueue.push_back(frame);
        queued = true;
      } else {
        ++droppedDecodePackets;
      }
      xSemaphoreGive(queueMutex);
    }
    if (queued) {
      if (decoderTask != nullptr) {
        xTaskNotifyGive(decoderTask);
      }
    } else if (droppedDecodePackets == 1 || droppedDecodePackets % 20 == 0) {
      Serial.printf("[audio] decode queue full; dropped=%lu\n",
                    static_cast<unsigned long>(droppedDecodePackets));
    }
  }

  bool isPlaybackIdle() {
    if (queueMutex == nullptr) {
      return true;
    }
    bool drained = false;
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
      drained = decodeQueue.empty() && playbackQueue.empty() &&
                !decodeInFlight && !outputInFlight;
      xSemaphoreGive(queueMutex);
    }
    return drained && millis() - lastPlaybackWriteMs > config.playbackIdleDelayMs;
  }
};

I2sOpusAudioPort::I2sOpusAudioPort(const Config& config) : impl_(new Impl(config)) {
  if (impl_->config.wire == nullptr) {
    impl_->config.wire = &Wire;
  }
}

I2sOpusAudioPort::~I2sOpusAudioPort() {
  end();
  delete impl_;
}

bool I2sOpusAudioPort::begin(const xiaozhi::AudioFormat& captureFormat, Uplink uplink) {
  const Config& config = impl_->config;
  const bool anyCodecCallback = config.codec.begin != nullptr ||
                                config.codec.setMuted != nullptr ||
                                config.codec.end != nullptr;
  const bool completeCodecCallbacks = config.codec.begin != nullptr &&
                                      config.codec.setMuted != nullptr &&
                                      config.codec.end != nullptr;
  const uint64_t hardwareSampleProduct =
      static_cast<uint64_t>(config.hardwareSampleRate) * captureFormat.frame_duration_ms;
  const uint64_t captureSampleProduct =
      static_cast<uint64_t>(captureFormat.sample_rate) * captureFormat.frame_duration_ms;
  const bool validFrameSize = hardwareSampleProduct % 1000 == 0 &&
                              captureSampleProduct % 1000 == 0;
  const bool validWakeConfig = !config.enableWakeDetection ||
                               (config.wakeModelPartition != nullptr &&
                                config.defaultWakeWord != nullptr &&
                                config.wakeTaskStackBytes > 0);
  const bool validHardwareConfig =
      config.hardwareSampleRate > 0 && config.i2sPort >= 0 && config.bclk >= 0 &&
      config.ws >= 0 && config.dataOut >= 0 && config.dataIn >= 0 &&
      (anyCodecCallback || !config.codecUseMclk || config.mclk >= 0) &&
      config.mclkMultiple > 0 &&
      config.dmaDescriptorCount > 0 && config.dmaFrames > 0 &&
      config.readFramesPerLoop > 0 && config.maximumDecodeQueueMs > 0 &&
      config.maximumPlaybackChunks > 0 && config.decoderTaskStackBytes > 0 &&
      config.outputTaskStackBytes > 0 && config.autoChannelSwitchRatio >= 1.0f &&
      !(config.captureChannel == CaptureChannel::Right && !config.stereo);
  const bool validBuiltInCodecConfig = anyCodecCallback ||
                                       (!config.initializeWire ||
                                        (config.i2cSda >= 0 && config.i2cScl >= 0));
  if (impl_->started || !uplink || captureFormat.sample_rate == 0 ||
      captureFormat.channels != 1 ||
      i2s_opus_detail::encoderDuration(captureFormat.frame_duration_ms) ==
          ESP_OPUS_ENC_FRAME_DURATION_ARG ||
      !validFrameSize || !validWakeConfig || !validHardwareConfig ||
      !validBuiltInCodecConfig || (anyCodecCallback && !completeCodecCallbacks)) {
    Serial.printf("[audio] invalid format or configuration: %lu Hz, %u channel(s), %u ms\n",
                  static_cast<unsigned long>(captureFormat.sample_rate), captureFormat.channels,
                  captureFormat.frame_duration_ms);
    Serial.println("[audio] check I2C/I2S pins, queue sizes, wake settings, and codec callbacks");
    return false;
  }
  impl_->i2sChannels = config.stereo ? 2 : 1;
  impl_->hardwareFramesPerPacket =
      static_cast<size_t>(hardwareSampleProduct / 1000);
  impl_->captureSamplesPerPacket = static_cast<size_t>(captureSampleProduct / 1000);
  impl_->maximumDecodePackets = std::max<size_t>(
      1, config.maximumDecodeQueueMs / captureFormat.frame_duration_ms);
  impl_->captured.resize(impl_->hardwareFramesPerPacket * impl_->i2sChannels);
  impl_->captureMono.resize(impl_->captureSamplesPerPacket);
  impl_->readBuffer.resize(config.readFramesPerLoop * impl_->i2sChannels);
  impl_->wakeCaptured.resize(impl_->captured.size());
  impl_->wakeMono.resize(impl_->captureSamplesPerPacket);
  impl_->captureFormat = captureFormat;
  impl_->uplink = std::move(uplink);
  if (!impl_->beginI2s() || !impl_->beginCodec() || !impl_->beginEncoder() ||
      !impl_->startWorkers() || !impl_->beginWakeDetection()) {
    impl_->stop();
    return false;
  }
  impl_->started = true;
  Serial.printf("[audio] I2S/Opus ready: hardware=%lu Hz capture=%lu Hz frame=%u ms\n",
                static_cast<unsigned long>(config.hardwareSampleRate),
                static_cast<unsigned long>(captureFormat.sample_rate),
                captureFormat.frame_duration_ms);
  return true;
}

void I2sOpusAudioPort::end() {
  if (impl_ != nullptr && impl_->started) {
    impl_->stop();
  }
}

void I2sOpusAudioPort::loop() {
  if (!impl_->started) {
    return;
  }
  impl_->pumpCapture();
}

void I2sOpusAudioPort::setCaptureEnabled(bool enabled) {
  if (!impl_->started || impl_->captureEnabled == enabled) {
    return;
  }
  impl_->captureEnabled = enabled;
  impl_->capturedSamples = 0;
  Serial.printf("[audio] capture %s\n", enabled ? "enabled" : "disabled");
}

void I2sOpusAudioPort::play(const xiaozhi::AudioFrame& frame) {
  if (impl_->started) {
    impl_->enqueueDecode(frame);
  }
}

bool I2sOpusAudioPort::playbackIdle() const {
  return impl_ == nullptr || !impl_->started ||
         impl_->isPlaybackIdle();
}

void I2sOpusAudioPort::setWakeDetectionEnabled(bool enabled) {
  if (impl_ != nullptr && impl_->started) {
    impl_->setWakeDetection(enabled);
  }
}

bool I2sOpusAudioPort::consumeWakeWord(std::string& wakeWord) {
  return impl_ != nullptr && impl_->started && impl_->consumeWake(wakeWord);
}
