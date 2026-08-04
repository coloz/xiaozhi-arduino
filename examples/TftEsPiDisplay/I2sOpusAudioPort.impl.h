#include "I2sOpusAudioPort.h"

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <soc/soc_caps.h>
#if XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM
#include <driver/i2s_pdm.h>
#if !defined(SOC_I2S_SUPPORTS_PDM_RX) || !SOC_I2S_SUPPORTS_PDM_RX
#error "Selected Xiaozhi audio profile requires an SoC with PDM RX support"
#endif
#endif
#if XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <model_path.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
#include <Wire.h>
#include <EspressifEs8311.h>
#include <es8311_reg.h>
#endif
#include <EspressifOpus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace i2s_opus_detail {

// Runtime-sized, allocation-free-after-reset FIFO. Audio queue capacities are
// configuration values, so a vector-backed ring keeps that flexibility while
// avoiding std::deque node churn in the packet hot paths.
template <typename T>
class FixedRingQueue {
 public:
  class ConstIterator {
   public:
    ConstIterator(const FixedRingQueue* queue, size_t offset)
        : queue_(queue), offset_(offset) {}
    const T& operator*() const {
      return queue_->slots_[(queue_->head_ + offset_) % queue_->slots_.size()];
    }
    ConstIterator& operator++() {
      ++offset_;
      return *this;
    }
    bool operator!=(const ConstIterator& other) const {
      return queue_ != other.queue_ || offset_ != other.offset_;
    }

   private:
    const FixedRingQueue* queue_;
    size_t offset_;
  };

  void reset(size_t capacity) {
    std::vector<T> fresh(capacity);
    slots_.swap(fresh);
    head_ = 0;
    count_ = 0;
  }

  void release() {
    std::vector<T>().swap(slots_);
    head_ = 0;
    count_ = 0;
  }

  void clear() {
    while (!empty()) {
      pop_front();
    }
    head_ = 0;
  }

  bool push_back(T&& value) {
    if (count_ >= slots_.size()) {
      return false;
    }
    const size_t tail = (head_ + count_) % slots_.size();
    slots_[tail] = std::move(value);
    ++count_;
    return true;
  }

  void pop_front() {
    if (empty()) {
      return;
    }
    slots_[head_] = T{};
    head_ = (head_ + 1) % slots_.size();
    --count_;
  }

  T& front() { return slots_[head_]; }
  const T& front() const { return slots_[head_]; }
  bool empty() const { return count_ == 0; }
  size_t size() const { return count_; }
  size_t capacity() const { return slots_.size(); }
  ConstIterator begin() const { return ConstIterator(this, 0); }
  ConstIterator end() const { return ConstIterator(this, count_); }

 private:
  std::vector<T> slots_;
  size_t head_ = 0;
  size_t count_ = 0;
};

esp_opus_enc_frame_duration_t encoderDuration(uint16_t durationMs) {
  switch (durationMs) {
    case 10:
      return ESP_OPUS_ENC_FRAME_DURATION_10_MS;
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
    case 10:
      return ESP_OPUS_DEC_FRAME_DURATION_10_MS;
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

#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
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
#endif

}  // namespace i2s_opus_detail

struct I2sOpusAudioPort::Impl {
  struct CaptureResamplerState {
    std::array<int16_t, 14> history{};
    char channel = '\0';
    bool initialized = false;
  };

  struct PlaybackResamplerState {
    std::array<int16_t, 30> history{};
    uint32_t sourceRate = 0;
    bool initialized = false;
    int16_t previousSample = 0;
    bool interpolationInitialized = false;
  };

  struct PlaybackChunk {
    std::vector<int16_t> pcm;
    uint32_t sourceRate = 0;
    uint32_t timestamp = 0;
    uint16_t durationMs = 0;
    uint8_t sourceChannels = 0;
    uint32_t generation = 0;
  };

  struct CaptureChunk {
    std::vector<int16_t> pcm;
    uint32_t timestamp = 0;
    uint32_t timestampGeneration = 0;
    uint32_t generation = 0;
    uint32_t peak = 0;
    uint32_t rms = 0;
    uint32_t rawRms = 0;
    float gain = 1.0f;
    char channel = 'L';
    bool gateOpen = false;
  };

  struct UplinkPacket {
    std::vector<uint8_t> opus;
    uint32_t timestamp = 0;
    uint32_t timestampGeneration = 0;
    uint32_t generation = 0;
  };

  struct DecodePacket {
    xiaozhi::AudioFrame frame;
    uint32_t generation = 0;
    bool resetDecoder = false;
  };

  explicit Impl(const Config& value) : config(value) {
    normalizeHardwareConfig();
  }

  Config config;
  xiaozhi::AudioFormat captureFormat{};
  Uplink uplink;
  bool started = false;
  std::atomic<bool> captureEnabled{false};
  std::atomic<bool> captureResetRequested{true};
  std::atomic<uint32_t> captureGeneration{1};

  i2s_chan_handle_t tx = nullptr;
  i2s_chan_handle_t rx = nullptr;
  SemaphoreHandle_t rxMutex = nullptr;
  SemaphoreHandle_t codecMutex = nullptr;
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
  i2s_opus_detail::WireCodecControl control{};
  const audio_codec_if_t* codec = nullptr;
#endif
  bool externalCodecStarted = false;
  void* encoder = nullptr;
  void* decoder = nullptr;
  uint32_t decoderRate = 0;
  uint16_t decoderDurationMs = 0;

  int encoderInputBytes = 0;
  int encoderOutputBytes = 0;
  size_t hardwareFramesPerPacket = 0;
  size_t captureSamplesPerPacket = 0;
  size_t inputChannels = 0;
  size_t outputChannels = 0;
  std::vector<int16_t> captured;
  std::vector<int16_t> readBuffer;
  std::vector<int32_t> readBuffer32;
  std::vector<int32_t> writeBuffer32;
  std::vector<int16_t> decodeScratch;
  size_t capturedSamples = 0;
  i2s_opus_detail::FixedRingQueue<CaptureChunk> encodeQueue;
  std::vector<std::vector<int16_t>> capturePool;
  i2s_opus_detail::FixedRingQueue<UplinkPacket> uplinkQueue;
  std::vector<std::vector<uint8_t>> uplinkPool;
  i2s_opus_detail::FixedRingQueue<DecodePacket> decodeQueue;
  i2s_opus_detail::FixedRingQueue<PlaybackChunk> playbackQueue;
  std::vector<std::vector<int16_t>> playbackPool;
  SemaphoreHandle_t queueMutex = nullptr;
  TaskHandle_t inputTask = nullptr;
  TaskHandle_t decoderTask = nullptr;
  TaskHandle_t outputTask = nullptr;
  std::atomic<bool> workersRunning{false};
  std::atomic<bool> inputTaskExited{true};
  std::atomic<bool> decoderTaskExited{true};
  std::atomic<bool> outputTaskExited{true};
  bool decodeInFlight = false;
  bool outputInFlight = false;
  std::atomic<uint32_t> playbackGeneration{1};
  std::atomic<bool> decoderResetRequested{false};
  std::atomic<uint32_t> lastPlaybackWriteMs{0};
  std::atomic<bool> playbackSuppressed{false};
  std::atomic<uint32_t> capturedPackets{0};
  std::atomic<uint32_t> latestCapturePeak{0};
  std::atomic<uint32_t> latestCaptureRms{0};
  std::atomic<uint32_t> latestCaptureRawRms{0};
  std::atomic<uint32_t> latestCaptureOpusBytes{0};
  bool speechGateOpen = false;
  uint8_t speechStartPackets = 0;
  uint16_t speechHoldPackets = 0;
  std::atomic<uint32_t> playedPackets{0};
  std::atomic<uint32_t> droppedDecodePackets{0};
  std::atomic<uint32_t> droppedEncodePackets{0};
  std::atomic<uint32_t> droppedUplinkPackets{0};
  std::atomic<uint32_t> discardedStaleCapturePackets{0};
  std::atomic<uint32_t> rejectedUplinkPackets{0};
  std::atomic<uint32_t> inputErrors{0};
  std::atomic<uint32_t> encodeErrors{0};
  std::atomic<uint32_t> decodeErrors{0};
  std::atomic<uint32_t> outputErrors{0};
  std::atomic<bool> playbackMuted{true};
  int32_t speakerGainQ15 = 22938;
  uint32_t lastPerformanceLogMs = 0;
  std::atomic<uint32_t> maximumEncodeUs{0};
  std::atomic<uint32_t> maximumDecodeUs{0};
  std::atomic<uint32_t> maximumWakeDetectUs{0};
  bool wakeDetectionStarted = false;
  // A detected word and the control task can both transition wake state. Keep
  // enabled + generation + event publication indivisible so an old detect
  // cannot disable a newly enabled generation (disable -> enable ABA).
  portMUX_TYPE wakeStateMux = portMUX_INITIALIZER_UNLOCKED;
  std::atomic<bool> wakeDetectionEnabled{false};
  std::atomic<bool> wakeResetRequested{true};
  std::atomic<uint32_t> wakeGeneration{1};
  std::atomic<bool> wakeDetected{false};
  std::atomic<uint32_t> wakeDetectedGeneration{0};
  std::atomic<bool> wakeTaskRunning{false};
  std::atomic<bool> wakeTaskExited{true};
  TaskHandle_t wakeTask = nullptr;
#if XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR
  srmodel_list_t* wakeModels = nullptr;
  const esp_wn_iface_t* wakeNet = nullptr;
  model_iface_data_t* wakeNetData = nullptr;
#endif
  size_t wakeChunkSamples = 0;
  char lastWakeWord[96]{};
  std::vector<int16_t> wakeCaptured;
  std::vector<int16_t> wakeMono;
  std::vector<int16_t> wakeDetectChunk;
  std::vector<int16_t> wakePcm;
  CaptureResamplerState captureResampler;
  CaptureResamplerState wakeResampler;
  PlaybackResamplerState playbackResampler;
  std::array<int16_t, 15> captureAntiAliasQ15{};
  std::array<int16_t, 31> playbackAntiAliasQ15{};
  static constexpr size_t kMaximumPlaybackTimestamps = 3;
  struct PlaybackTimestamp {
    uint32_t value = 0;
    uint32_t generation = 0;
  };
  std::array<PlaybackTimestamp, kMaximumPlaybackTimestamps> playbackTimestamps{};
  size_t playbackTimestampHead = 0;
  size_t playbackTimestampCount = 0;

  void normalizeHardwareConfig() {
    if (config.useLegacyHardwareConfig) {
      HardwareConfig& hardware = config.hardware;
      hardware.inputMode = InputMode::I2sStandard;
      hardware.busMode = BusMode::Shared;
      hardware.codecMode =
          config.codec.begin == nullptr ? CodecMode::Es8311 : CodecMode::Custom;

      StandardI2sEndpoint endpoint;
      endpoint.port = config.i2sPort;
      endpoint.sampleRate = config.hardwareSampleRate;
      endpoint.mclk = config.mclk;
      endpoint.bclk = config.bclk;
      endpoint.ws = config.ws;
      endpoint.invertMclk = config.invertMclk;
      endpoint.invertBclk = config.invertBclk;
      endpoint.invertWs = config.invertWs;
      endpoint.dataBits = 16;
      endpoint.validBits = 16;
      endpoint.slotBits = 16;
      endpoint.channels = config.stereo ? 2 : 1;
      endpoint.slot = config.stereo ? I2sSlot::Both : I2sSlot::Left;
      hardware.input = endpoint;
      hardware.output = endpoint;
      hardware.input.data = config.dataIn;
      hardware.output.data = config.dataOut;

      Es8311Control& es8311 = hardware.es8311;
      es8311.wire = config.wire;
      es8311.initializeWire = config.initializeWire;
      es8311.i2cSda = config.i2cSda;
      es8311.i2cScl = config.i2cScl;
      es8311.i2cFrequency = config.i2cFrequency;
      es8311.address = config.codecAddress;
      es8311.resetOnBegin = config.resetCodecOnBegin;
      es8311.resetDelayMs = config.codecResetDelayMs;
      es8311.paPin = config.codecPaPin;
      es8311.useMclk = config.codecUseMclk;
      es8311.master = config.codecMaster;
      es8311.noDacReference = config.codecNoDacReference;
      es8311.paSupplyVoltage = config.paSupplyVoltage;
      es8311.codecDacVoltage = config.codecDacVoltage;
      es8311.paGainDb = config.paGainDb;
      es8311.microphoneGainDb = config.microphoneGainDb;
      es8311.outputVolumeDb = config.outputVolumeDb;
    }
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
    if (config.hardware.es8311.wire == nullptr) {
      config.hardware.es8311.wire = &Wire;
    }
#endif
    const uint8_t volume =
        std::min<uint8_t>(100, config.hardware.amplifier.volumePercent);
    speakerGainQ15 = static_cast<int32_t>(volume) * 32768 / 100;
  }

  uint32_t inputSampleRate() const {
    return config.hardware.inputMode == InputMode::I2sPdm
               ? config.hardware.pdmInput.sampleRate
               : config.hardware.input.sampleRate;
  }

  uint32_t outputSampleRate() const {
    return config.hardware.output.sampleRate;
  }

  static bool validStandardEndpoint(const StandardI2sEndpoint& endpoint,
                                    bool input) {
    const bool validDataWidth =
        endpoint.dataBits == 16 || endpoint.dataBits == 32;
    const bool validSlot = endpoint.slotBits == 16 || endpoint.slotBits == 24 ||
                           endpoint.slotBits == 32;
    const bool validSlotSelection =
        endpoint.slot == I2sSlot::Left || endpoint.slot == I2sSlot::Right ||
        endpoint.slot == I2sSlot::Both;
    // ESP-IDF mono mode may duplicate one DMA sample into BOTH wire slots.
    // Stereo DMA must expose both slots because the pipeline consumes two
    // samples per frame.
    const bool validChannelPacking =
        endpoint.channels == 1 || endpoint.slot == I2sSlot::Both;
    const bool validPacking = input
                                  ? (endpoint.dataBits == 32 ||
                                     (endpoint.validBits == 16 &&
                                      endpoint.rightShift == 0))
                                  : endpoint.validBits == 16;
    const int pins[] = {endpoint.mclk, endpoint.bclk, endpoint.ws,
                        endpoint.data};
    bool distinctPins = true;
    for (size_t left = 0; left < 4; ++left) {
      if (pins[left] < 0) {
        continue;
      }
      for (size_t right = left + 1; right < 4; ++right) {
        if (pins[left] == pins[right]) {
          distinctPins = false;
        }
      }
    }
    return endpoint.port >= 0 && endpoint.sampleRate > 0 &&
           endpoint.sampleRate % 100 == 0 && endpoint.bclk >= 0 &&
           endpoint.ws >= 0 && endpoint.data >= 0 && validDataWidth &&
           validSlot && validSlotSelection && validChannelPacking &&
           distinctPins &&
           validPacking &&
           endpoint.validBits > 0 && endpoint.validBits <= endpoint.dataBits &&
           endpoint.slotBits >= endpoint.dataBits &&
           (endpoint.channels == 1 || endpoint.channels == 2) &&
           (!input || endpoint.rightShift < 32);
  }

  static bool standardEndpointsOverlap(const StandardI2sEndpoint& first,
                                       const StandardI2sEndpoint& second) {
    const int firstPins[] = {first.mclk, first.bclk, first.ws, first.data};
    const int secondPins[] = {second.mclk, second.bclk, second.ws, second.data};
    for (int firstPin : firstPins) {
      if (firstPin < 0) {
        continue;
      }
      for (int secondPin : secondPins) {
        if (firstPin == secondPin) {
          return true;
        }
      }
    }
    return false;
  }

  static bool standardEndpointUsesPin(const StandardI2sEndpoint& endpoint,
                                      int pin) {
    return pin >= 0 &&
           (endpoint.mclk == pin || endpoint.bclk == pin ||
            endpoint.ws == pin || endpoint.data == pin);
  }

  static bool outputOverlapsPdm(const StandardI2sEndpoint& output,
                                const PdmInputEndpoint& input) {
    const int outputPins[] = {output.mclk, output.bclk, output.ws, output.data};
    for (int outputPin : outputPins) {
      if (outputPin >= 0 &&
          (outputPin == input.clock || outputPin == input.data)) {
        return true;
      }
    }
    return false;
  }

  bool validateHardwareConfig(std::string& error) const {
    const HardwareConfig& hardware = config.hardware;
#if !XIAOZHI_AUDIO_ENABLE_OUTPUT_I2S_STD
    error = "selected profile did not compile the standard-I2S output backend";
    return false;
#endif
    if (!validStandardEndpoint(hardware.output, false)) {
      error = "invalid standard-I2S speaker endpoint";
      return false;
    }

    if (hardware.inputMode == InputMode::I2sStandard) {
#if !XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD
      error = "standard-I2S microphone backend was not compiled";
      return false;
#endif
      if (!validStandardEndpoint(hardware.input, true)) {
        error = "invalid standard-I2S microphone endpoint";
        return false;
      }
    } else {
#if !XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM
      error = "PDM microphone backend was not compiled";
      return false;
#endif
      if (hardware.pdmInput.port < 0 ||
          hardware.pdmInput.sampleRate == 0 ||
          hardware.pdmInput.sampleRate % 100 != 0 ||
          hardware.pdmInput.clock < 0 || hardware.pdmInput.data < 0 ||
          hardware.pdmInput.clock == hardware.pdmInput.data) {
        error = "invalid PDM microphone endpoint";
        return false;
      }
    }

#ifdef SOC_I2S_NUM
    const int inputPort = hardware.inputMode == InputMode::I2sPdm
                              ? hardware.pdmInput.port
                              : hardware.input.port;
    if (hardware.output.port >= SOC_I2S_NUM || inputPort >= SOC_I2S_NUM) {
      error = "selected I2S controller does not exist on this SoC";
      return false;
    }
#endif

    if (hardware.busMode == BusMode::Shared) {
#if !XIAOZHI_AUDIO_ENABLE_I2S_SHARED
      error = "shared I2S backend was not compiled";
      return false;
#else
      const StandardI2sEndpoint& input = hardware.input;
      const StandardI2sEndpoint& output = hardware.output;
      if (hardware.inputMode != InputMode::I2sStandard ||
          input.port != output.port || input.sampleRate != output.sampleRate ||
          input.mclk != output.mclk || input.bclk != output.bclk ||
          input.ws != output.ws || input.invertMclk != output.invertMclk ||
          input.invertBclk != output.invertBclk ||
          input.invertWs != output.invertWs ||
          input.dataBits != output.dataBits ||
          input.slotBits != output.slotBits ||
          input.channels != output.channels || input.data == output.data) {
        error = "shared I2S requires identical RX/TX clocks, rate, width and channel mode";
        return false;
      }
#endif
    } else {
#if !XIAOZHI_AUDIO_ENABLE_I2S_SEPARATE
      error = "separate I2S backend was not compiled";
      return false;
#else
      const int inputPort = hardware.inputMode == InputMode::I2sPdm
                                ? hardware.pdmInput.port
                                : hardware.input.port;
      if (inputPort == hardware.output.port) {
        error = "separate microphone/speaker mode requires different I2S controllers";
        return false;
      }
      if ((hardware.inputMode == InputMode::I2sStandard &&
           standardEndpointsOverlap(hardware.input, hardware.output)) ||
          (hardware.inputMode == InputMode::I2sPdm &&
           outputOverlapsPdm(hardware.output, hardware.pdmInput))) {
        error = "separate microphone/speaker endpoints must not share GPIOs";
        return false;
      }
#endif
    }

    const bool anyCodecCallback = config.codec.begin != nullptr ||
                                  config.codec.setMuted != nullptr ||
                                  config.codec.end != nullptr;
    const bool completeCodecCallbacks = config.codec.begin != nullptr &&
                                        config.codec.setMuted != nullptr &&
                                        config.codec.end != nullptr;
    if (anyCodecCallback != completeCodecCallbacks) {
      error = "custom codec requires begin, setMuted and end callbacks";
      return false;
    }
    if (hardware.codecMode == CodecMode::Custom && !completeCodecCallbacks) {
      error = "custom codec profile has no complete codec callbacks";
      return false;
    }
    if (hardware.codecMode != CodecMode::Custom && anyCodecCallback) {
      error = "codec callbacks require CodecMode::Custom";
      return false;
    }
    if (hardware.codecMode == CodecMode::Es8311) {
#if !XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
      error = "ES8311 backend was not compiled";
      return false;
#else
      const Es8311Control& es8311 = hardware.es8311;
      const bool invalidI2c =
          es8311.initializeWire &&
          (es8311.i2cSda < 0 || es8311.i2cScl < 0 ||
           es8311.i2cSda == es8311.i2cScl || es8311.i2cFrequency == 0 ||
           standardEndpointUsesPin(hardware.input, es8311.i2cSda) ||
           standardEndpointUsesPin(hardware.output, es8311.i2cSda) ||
           standardEndpointUsesPin(hardware.input, es8311.i2cScl) ||
           standardEndpointUsesPin(hardware.output, es8311.i2cScl) ||
           es8311.i2cSda == es8311.paPin ||
           es8311.i2cScl == es8311.paPin);
      const bool invalidPaPin =
          es8311.paPin < -1 ||
          (es8311.paPin >= 0 &&
           (standardEndpointUsesPin(hardware.input, es8311.paPin) ||
            standardEndpointUsesPin(hardware.output, es8311.paPin) ||
            es8311.paPin == es8311.i2cSda ||
            es8311.paPin == es8311.i2cScl));
      if (hardware.busMode != BusMode::Shared ||
          inputSampleRate() != outputSampleRate() ||
          hardware.input.dataBits != 16 || hardware.output.dataBits != 16 ||
          es8311.wire == nullptr ||
          es8311.master ||
          invalidI2c ||
          invalidPaPin ||
          (es8311.useMclk && hardware.output.mclk < 0)) {
        error = "ES8311 requires codec-slave control and shared 16-bit I2S";
        return false;
      }
#endif
    }
    if (hardware.codecMode == CodecMode::None) {
      const AmplifierControl& amplifier = hardware.amplifier;
      const bool amplifierPinOverlaps =
          amplifier.enablePin >= 0 &&
          (standardEndpointUsesPin(hardware.output, amplifier.enablePin) ||
           (hardware.inputMode == InputMode::I2sStandard
                ? standardEndpointUsesPin(hardware.input, amplifier.enablePin)
                : (amplifier.enablePin == hardware.pdmInput.clock ||
                   amplifier.enablePin == hardware.pdmInput.data)));
      if (amplifier.volumePercent > 100 || amplifier.enablePin < -1 ||
          amplifierPinOverlaps) {
        error = "invalid software volume or amplifier enable GPIO";
        return false;
      }
    }
    if (config.enableWakeDetection && !XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR) {
      error = "wake detection requested but ESP-SR backend was not compiled";
      return false;
    }
    return true;
  }

  static void updateMaximum(std::atomic<uint32_t>& maximum, uint32_t value) {
    uint32_t previous = maximum.load(std::memory_order_relaxed);
    while (value > previous &&
           !maximum.compare_exchange_weak(previous, value,
                                          std::memory_order_relaxed)) {
    }
  }

  void clearPlaybackTimestampsLocked() {
    playbackTimestampHead = 0;
    playbackTimestampCount = 0;
  }

  void pushPlaybackTimestampLocked(uint32_t timestamp, uint32_t generation) {
    if (timestamp == 0) {
      return;
    }
    if (playbackTimestampCount == kMaximumPlaybackTimestamps) {
      playbackTimestampHead =
          (playbackTimestampHead + 1) % kMaximumPlaybackTimestamps;
      --playbackTimestampCount;
    }
    const size_t tail =
        (playbackTimestampHead + playbackTimestampCount) %
        kMaximumPlaybackTimestamps;
    playbackTimestamps[tail] = PlaybackTimestamp{timestamp, generation};
    ++playbackTimestampCount;
  }

  PlaybackTimestamp popPlaybackTimestampLocked() {
    const uint32_t generation = playbackGeneration.load();
    while (playbackTimestampCount != 0) {
      const PlaybackTimestamp item = playbackTimestamps[playbackTimestampHead];
      playbackTimestampHead =
          (playbackTimestampHead + 1) % kMaximumPlaybackTimestamps;
      --playbackTimestampCount;
      if (item.generation == generation) {
        return item;
      }
    }
    return {};
  }

  template <size_t kTapCount>
  static void initializeAntiAliasFilter(
      uint32_t sourceRate, uint32_t destinationRate,
      std::array<int16_t, kTapCount>& output) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int center = static_cast<int>(kTapCount / 2);
    const double rateRatio = std::min(
        1.0, static_cast<double>(destinationRate) /
                 static_cast<double>(sourceRate));
    const double cutoff = 0.45 * rateRatio;
    std::array<double, kTapCount> coefficients{};
    double sum = 0.0;
    for (size_t index = 0; index < kTapCount; ++index) {
      const int offset = static_cast<int>(index) - center;
      const double sinc = offset == 0
                              ? 2.0 * cutoff
                              : std::sin(2.0 * kPi * cutoff * offset) /
                                    (kPi * offset);
      const double window =
          0.54 - 0.46 * std::cos(2.0 * kPi * index / (kTapCount - 1));
      coefficients[index] = sinc * window;
      sum += coefficients[index];
    }
    int32_t quantizedSum = 0;
    for (size_t index = 0; index < kTapCount; ++index) {
      output[index] = static_cast<int16_t>(
          std::lround(coefficients[index] * 32768.0 / sum));
      quantizedSum += output[index];
    }
    output[center] = static_cast<int16_t>(
        output[center] + (32768 - quantizedSum));
  }

  void initializeCaptureFilter() {
    initializeAntiAliasFilter(inputSampleRate(),
                              captureFormat.sample_rate,
                              captureAntiAliasQ15);
  }

  static i2s_data_bit_width_t dataBitWidth(uint8_t bits) {
    switch (bits) {
      case 16:
        return I2S_DATA_BIT_WIDTH_16BIT;
      case 24:
        return I2S_DATA_BIT_WIDTH_24BIT;
      case 32:
        return I2S_DATA_BIT_WIDTH_32BIT;
      default:
        return I2S_DATA_BIT_WIDTH_16BIT;
    }
  }

  static i2s_slot_bit_width_t slotBitWidth(uint8_t bits) {
    switch (bits) {
      case 16:
        return I2S_SLOT_BIT_WIDTH_16BIT;
      case 24:
        return I2S_SLOT_BIT_WIDTH_24BIT;
      case 32:
        return I2S_SLOT_BIT_WIDTH_32BIT;
      default:
        return I2S_SLOT_BIT_WIDTH_AUTO;
    }
  }

  static i2s_std_slot_mask_t slotMask(I2sSlot slot) {
    switch (slot) {
      case I2sSlot::Right:
        return I2S_STD_SLOT_RIGHT;
      case I2sSlot::Both:
        return I2S_STD_SLOT_BOTH;
      case I2sSlot::Left:
      default:
        return I2S_STD_SLOT_LEFT;
    }
  }

  i2s_chan_config_t channelConfig(int port) const {
    i2s_chan_config_t result = I2S_CHANNEL_DEFAULT_CONFIG(
        static_cast<i2s_port_t>(port), I2S_ROLE_MASTER);
    result.dma_desc_num = config.dmaDescriptorCount;
    result.dma_frame_num = config.dmaFrames;
    result.auto_clear_after_cb = true;
    result.auto_clear_before_cb = false;
    return result;
  }

  i2s_std_config_t standardConfig(const StandardI2sEndpoint& endpoint,
                                  bool includeOutput,
                                  bool includeInput) const {
    i2s_std_config_t standard{};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(endpoint.sampleRate);
    standard.clk_cfg.mclk_multiple =
        static_cast<i2s_mclk_multiple_t>(config.mclkMultiple);
    standard.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        dataBitWidth(endpoint.dataBits),
        endpoint.channels == 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
    standard.slot_cfg.slot_bit_width = slotBitWidth(endpoint.slotBits);
    standard.slot_cfg.ws_width = endpoint.slotBits;
    standard.slot_cfg.slot_mask = slotMask(endpoint.slot);
    standard.gpio_cfg.mclk = static_cast<gpio_num_t>(endpoint.mclk);
    standard.gpio_cfg.bclk = static_cast<gpio_num_t>(endpoint.bclk);
    standard.gpio_cfg.ws = static_cast<gpio_num_t>(endpoint.ws);
    standard.gpio_cfg.dout = static_cast<gpio_num_t>(
        includeOutput ? config.hardware.output.data : -1);
    standard.gpio_cfg.din = static_cast<gpio_num_t>(
        includeInput ? config.hardware.input.data : -1);
    standard.gpio_cfg.invert_flags.mclk_inv = endpoint.invertMclk;
    standard.gpio_cfg.invert_flags.bclk_inv = endpoint.invertBclk;
    standard.gpio_cfg.invert_flags.ws_inv = endpoint.invertWs;
    return standard;
  }

  bool beginI2s() {
    rxMutex = xSemaphoreCreateMutex();
    codecMutex = xSemaphoreCreateMutex();
    if (rxMutex == nullptr || codecMutex == nullptr) {
      Serial.println("[audio] failed to create I2S/codec mutexes");
      return false;
    }
    const HardwareConfig& hardware = config.hardware;
    if (hardware.busMode == BusMode::Shared) {
#if XIAOZHI_AUDIO_ENABLE_I2S_SHARED && \
    XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD && \
    XIAOZHI_AUDIO_ENABLE_OUTPUT_I2S_STD
      i2s_chan_config_t sharedChannel = channelConfig(hardware.output.port);
      if (i2s_new_channel(&sharedChannel, &tx, &rx) != ESP_OK) {
        Serial.println("[audio] failed to allocate shared I2S channels");
        return false;
      }
      i2s_std_config_t outputStandard =
          standardConfig(hardware.output, true, false);
      i2s_std_config_t inputStandard =
          standardConfig(hardware.input, false, true);
      if (i2s_channel_init_std_mode(tx, &outputStandard) != ESP_OK ||
          i2s_channel_init_std_mode(rx, &inputStandard) != ESP_OK ||
          i2s_channel_enable(tx) != ESP_OK ||
          i2s_channel_enable(rx) != ESP_OK) {
        Serial.println("[audio] failed to initialize shared standard I2S");
        return false;
      }
      return true;
#else
      Serial.println("[audio] shared standard-I2S backend was not compiled");
      return false;
#endif
    }

#if XIAOZHI_AUDIO_ENABLE_I2S_SEPARATE
#if XIAOZHI_AUDIO_ENABLE_OUTPUT_I2S_STD
    i2s_chan_config_t outputChannel = channelConfig(hardware.output.port);
    if (i2s_new_channel(&outputChannel, &tx, nullptr) != ESP_OK) {
      Serial.println("[audio] failed to allocate speaker I2S channel");
      return false;
    }
    i2s_std_config_t outputStandard =
        standardConfig(hardware.output, true, false);
    if (i2s_channel_init_std_mode(tx, &outputStandard) != ESP_OK) {
      Serial.println("[audio] failed to initialize speaker standard I2S");
      return false;
    }
#else
    Serial.println("[audio] standard-I2S speaker backend was not compiled");
    return false;
#endif

    if (hardware.inputMode == InputMode::I2sStandard) {
#if XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD
      i2s_chan_config_t inputChannel = channelConfig(hardware.input.port);
      if (i2s_new_channel(&inputChannel, nullptr, &rx) != ESP_OK) {
        Serial.println("[audio] failed to allocate microphone I2S channel");
        return false;
      }
      i2s_std_config_t inputStandard =
          standardConfig(hardware.input, false, true);
      if (i2s_channel_init_std_mode(rx, &inputStandard) != ESP_OK) {
        Serial.println("[audio] failed to initialize microphone standard I2S");
        return false;
      }
#else
      Serial.println("[audio] standard-I2S microphone backend was not compiled");
      return false;
#endif
    } else {
#if XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM
      i2s_chan_config_t inputChannel = channelConfig(hardware.pdmInput.port);
      if (i2s_new_channel(&inputChannel, nullptr, &rx) != ESP_OK) {
        Serial.println("[audio] failed to allocate PDM microphone channel");
        return false;
      }
      i2s_pdm_rx_config_t pdm{};
      pdm.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(
          hardware.pdmInput.sampleRate);
      pdm.slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
      pdm.gpio_cfg.clk = static_cast<gpio_num_t>(hardware.pdmInput.clock);
      pdm.gpio_cfg.din = static_cast<gpio_num_t>(hardware.pdmInput.data);
      pdm.gpio_cfg.invert_flags.clk_inv = hardware.pdmInput.invertClock;
      if (i2s_channel_init_pdm_rx_mode(rx, &pdm) != ESP_OK) {
        Serial.println("[audio] failed to initialize PDM microphone");
        return false;
      }
#else
      Serial.println("[audio] PDM microphone backend was not compiled");
      return false;
#endif
    }

    if (i2s_channel_enable(tx) != ESP_OK ||
        i2s_channel_enable(rx) != ESP_OK) {
      Serial.println("[audio] failed to enable separate I2S channels");
      return false;
    }
    return true;
#else
    Serial.println("[audio] separate I2S backend was not compiled");
    return false;
#endif
  }

  bool setCodecMuted(bool muted) {
    if (externalCodecStarted) {
      return config.codec.setMuted(config.codec.context, muted);
    }
    if (config.hardware.codecMode == CodecMode::None) {
#if XIAOZHI_AUDIO_ENABLE_AMP_GPIO
      const AmplifierControl& amplifier = config.hardware.amplifier;
      if (amplifier.enablePin >= 0) {
        digitalWrite(amplifier.enablePin,
                     muted ? !amplifier.activeLevel : amplifier.activeLevel);
      }
#endif
      return true;
    }
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
    if (codec == nullptr) {
      return false;
    }
    const Es8311Control& es8311 = config.hardware.es8311;
    if (muted && es8311.paPin >= 0) {
      digitalWrite(es8311.paPin, !es8311.paActiveLevel);
    }
    const bool changed = codec->mute(codec, muted) == ESP_CODEC_DEV_OK;
    if (changed && !muted && es8311.paPin >= 0) {
      digitalWrite(es8311.paPin, es8311.paActiveLevel);
    }
    return changed;
#else
    return false;
#endif
  }

  bool applyCodecMute(bool muted) {
    if (codecMutex == nullptr ||
        xSemaphoreTake(codecMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      return false;
    }
    const bool changed = setCodecMuted(muted);
    if (changed) {
      playbackMuted.store(muted);
    }
    xSemaphoreGive(codecMutex);
    return changed;
  }

  bool beginCodec() {
    CodecFormat format;
    format.sampleRate = outputSampleRate();
    format.bitsPerSample = config.hardware.output.dataBits;
    format.channels = static_cast<uint8_t>(outputChannels);
    format.mclkMultiple = config.mclkMultiple;
    format.inputSampleRate = inputSampleRate();
    format.outputSampleRate = outputSampleRate();
    format.inputChannels = static_cast<uint8_t>(inputChannels);
    format.outputChannels = static_cast<uint8_t>(outputChannels);

    if (config.hardware.codecMode == CodecMode::Custom) {
      externalCodecStarted = config.codec.begin(config.codec.context, format);
      if (!externalCodecStarted) {
        Serial.println("[audio] external codec initialization failed");
        return false;
      }
      if (!setCodecMuted(true)) {
        Serial.println("[audio] external codec initial mute failed");
        return false;
      }
      playbackMuted.store(true);
      return true;
    }

    if (config.hardware.codecMode == CodecMode::None) {
#if XIAOZHI_AUDIO_ENABLE_AMP_GPIO
      if (config.hardware.amplifier.enablePin >= 0) {
        pinMode(config.hardware.amplifier.enablePin, OUTPUT);
      }
#endif
      if (!setCodecMuted(true)) {
        Serial.println("[audio] failed to place the speaker amplifier in standby");
        return false;
      }
      playbackMuted.store(true);
      return true;
    }

#if !XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
    Serial.println("[audio] ES8311 backend was not compiled for this profile");
    return false;
#else
    Es8311Control& es8311 = config.hardware.es8311;

    if (es8311.paPin >= 0) {
      pinMode(es8311.paPin, OUTPUT);
      digitalWrite(es8311.paPin, !es8311.paActiveLevel);
    }

    if (es8311.initializeWire &&
        !es8311.wire->begin(es8311.i2cSda, es8311.i2cScl,
                            es8311.i2cFrequency)) {
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
    if (es8311.resetOnBegin) {
      for (const auto& item : resetSequence) {
        es8311.wire->beginTransmission(es8311.address);
        es8311.wire->write(item.first);
        es8311.wire->write(item.second);
        if (es8311.wire->endTransmission() != 0) {
          Serial.printf("[audio] codec reset write failed at register 0x%02X\n",
                        item.first);
          return false;
        }
      }
      delay(es8311.resetDelayMs);
    }

    control.base.open = i2s_opus_detail::wireOpen;
    control.base.is_open = i2s_opus_detail::wireIsOpen;
    control.base.read_reg = i2s_opus_detail::wireRead;
    control.base.write_reg = i2s_opus_detail::wireWrite;
    control.base.close = i2s_opus_detail::wireClose;
    control.wire = es8311.wire;
    control.address = es8311.address;
    control.opened = true;

    es8311_codec_cfg_t codecConfig{};
    codecConfig.ctrl_if = &control.base;
    codecConfig.gpio_if = nullptr;
    codecConfig.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    // PA GPIO is managed alongside runtime mute above. The bundled ES8311
    // driver only toggles its own PA pin on codec enable/disable, which would
    // leave the amplifier powered while idle.
    codecConfig.pa_pin = -1;
    codecConfig.use_mclk = es8311.useMclk;
    codecConfig.master_mode = es8311.master;
    codecConfig.no_dac_ref = es8311.noDacReference;
    codecConfig.mclk_div = config.mclkMultiple;
    codecConfig.hw_gain.pa_voltage = es8311.paSupplyVoltage;
    codecConfig.hw_gain.codec_dac_voltage = es8311.codecDacVoltage;
    codecConfig.hw_gain.pa_gain = es8311.paGainDb;
    codec = es8311_codec_new(&codecConfig);
    if (codec == nullptr) {
      Serial.println("[audio] built-in codec initialization failed");
      return false;
    }

    esp_codec_dev_sample_info_t sampleInfo{};
    sampleInfo.bits_per_sample = 16;
    sampleInfo.channel = static_cast<uint8_t>(outputChannels);
    sampleInfo.channel_mask = outputChannels == 2 ? 0x03 : 0x01;
    sampleInfo.sample_rate = outputSampleRate();
    sampleInfo.mclk_multiple = config.mclkMultiple;
    const int fsResult = codec->set_fs(codec, &sampleInfo);
    const int gainResult =
        codec->set_mic_gain(codec, es8311.microphoneGainDb);
    const int volumeResult = codec->set_vol(codec, es8311.outputVolumeDb);
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
    playbackMuted.store(true);
    return true;
#endif
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
    return true;
  }

  void initializeBufferPools() {
    encodeQueue.reset(config.maximumEncodePackets);
    capturePool.clear();
    uplinkQueue.reset(config.maximumUplinkPackets);
    uplinkPool.clear();
    // Protocol and bundled codec both accept 10/20/40/60 ms frames. Two spare
    // slots leave room for the decoder task and a concurrently arriving frame.
    decodeQueue.reset(
        std::max<size_t>(2, config.maximumDecodeQueueMs / 10 + 2));
    playbackQueue.reset(config.maximumPlaybackChunks);
    playbackPool.clear();
    clearPlaybackTimestampsLocked();
    capturedPackets.store(0);
    playedPackets.store(0);
    droppedDecodePackets.store(0);
    droppedEncodePackets.store(0);
    droppedUplinkPackets.store(0);
    discardedStaleCapturePackets.store(0);
    rejectedUplinkPackets.store(0);
    inputErrors.store(0);
    encodeErrors.store(0);
    decodeErrors.store(0);
    outputErrors.store(0);
    maximumEncodeUs.store(0);
    maximumDecodeUs.store(0);
    maximumWakeDetectUs.store(0);
    latestCapturePeak.store(0);
    latestCaptureRms.store(0);
    latestCaptureRawRms.store(0);
    latestCaptureOpusBytes.store(0);
    lastPerformanceLogMs = millis();

    capturePool.reserve(config.maximumEncodePackets + 1);
    uplinkPool.reserve(config.maximumUplinkPackets + 1);
    playbackPool.reserve(config.maximumPlaybackChunks + 1);
    for (size_t index = 0; index < config.maximumEncodePackets + 1; ++index) {
      capturePool.emplace_back(captureSamplesPerPacket);
    }
    for (size_t index = 0; index < config.maximumUplinkPackets + 1; ++index) {
      uplinkPool.emplace_back(static_cast<size_t>(encoderOutputBytes));
    }
    const size_t maximumPlaybackSamples =
        static_cast<size_t>(outputSampleRate()) * 60 / 1000 * outputChannels;
    for (size_t index = 0; index < config.maximumPlaybackChunks + 1; ++index) {
      std::vector<int16_t> buffer;
      buffer.reserve(maximumPlaybackSamples);
      playbackPool.push_back(std::move(buffer));
    }
    // Both the protocol and our Opus decoder are mono. Size for the largest
    // accepted 48 kHz/60 ms frame without retaining an unused second channel.
    decodeScratch.resize(48000 * 60 / 1000 + 16);
  }

#if XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR
  static void wakeTaskEntry(void* context) {
    static_cast<Impl*>(context)->wakeLoop();
  }

  void wakeLoop() {
    while (wakeTaskRunning.load()) {
      if (!wakeDetectionEnabled.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        continue;
      }

      const uint32_t generation = wakeGeneration.load();
      if (wakeResetRequested.exchange(false)) {
        // These objects and WakeNet's internal history are task-owned. Control
        // code only publishes a reset request, avoiding concurrent clear/detect.
        wakePcm.clear();
        wakeResampler.initialized = false;
        if (wakeNet != nullptr && wakeNetData != nullptr && wakeNet->clean != nullptr) {
          wakeNet->clean(wakeNetData);
        }
      }

      size_t inputSamples = 0;
      esp_err_t result = ESP_ERR_TIMEOUT;
      if (rxMutex != nullptr &&
          xSemaphoreTake(rxMutex, pdMS_TO_TICKS(25)) == pdTRUE) {
        if (wakeDetectionEnabled.load() && generation == wakeGeneration.load()) {
          result = readCaptureSamples(wakeCaptured.data(), wakeCaptured.size(),
                                      inputSamples, pdMS_TO_TICKS(20));
        }
        xSemaphoreGive(rxMutex);
      }
      if (!wakeTaskRunning.load() || !wakeDetectionEnabled.load() ||
          generation != wakeGeneration.load()) {
        continue;
      }
      if (result != ESP_OK || inputSamples != wakeCaptured.size()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }

      char channel = 'L';
      uint32_t peak = 0;
      uint32_t rms = 0;
      resampleCapture(wakeCaptured.data(), wakeCaptured.size() / inputChannels,
                      wakeMono, wakeResampler, channel, peak, rms, false);
      wakePcm.insert(wakePcm.end(), wakeMono.begin(), wakeMono.end());

      while (wakeDetectionEnabled.load() &&
             generation == wakeGeneration.load() &&
             wakePcm.size() >= wakeChunkSamples) {
        std::copy_n(wakePcm.data(), wakeChunkSamples, wakeDetectChunk.data());
        // One bounded compaction per WakeNet chunk replaces hundreds of deque
        // pop_front calls and avoids its long-running block allocator churn.
        wakePcm.erase(wakePcm.begin(), wakePcm.begin() + wakeChunkSamples);
        const uint32_t detectStartedUs = micros();
        const int detected = wakeNet->detect(wakeNetData, wakeDetectChunk.data());
        updateMaximum(maximumWakeDetectUs, micros() - detectStartedUs);
        if (!wakeTaskRunning.load() || !wakeDetectionEnabled.load() ||
            generation != wakeGeneration.load()) {
          continue;
        }
        if (detected <= 0) {
          continue;
        }
        const char* word = wakeNet->get_word_name(wakeNetData, detected);
        snprintf(lastWakeWord, sizeof(lastWakeWord), "%s",
                 word == nullptr || word[0] == '\0' ? config.defaultWakeWord : word);
        bool committed = false;
        portENTER_CRITICAL(&wakeStateMux);
        if (wakeTaskRunning.load() && wakeDetectionEnabled.load() &&
            generation == wakeGeneration.load()) {
          wakeDetectionEnabled.store(false);
          wakeDetectedGeneration.store(generation);
          wakeDetected.store(true);
          committed = true;
        }
        portEXIT_CRITICAL(&wakeStateMux);
        if (!committed) {
          continue;
        }
        wakePcm.clear();
        break;
      }
    }
    wakeTaskExited.store(true);
    // Keep the handle valid until the owner observes the exit flag and deletes
    // the task. This removes the notify-versus-self-delete handle race.
    vTaskSuspend(nullptr);
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
    char* modelName = esp_srmodel_filter(
        wakeModels, ESP_WN_PREFIX, config.wakeModelKeyword);
    if (modelName == nullptr ||
        (wakeNet = static_cast<const esp_wn_iface_t*>(
             esp_wn_handle_from_name(modelName))) == nullptr ||
        (wakeNetData = wakeNet->create(
             modelName, config.wakeDetectionMode == WakeDetectionMode::Aggressive
                            ? DET_MODE_95
                            : DET_MODE_90)) == nullptr) {
      Serial.printf("[wake] WakeNet model initialization failed (wanted=%s)\n",
                    config.wakeModelKeyword == nullptr
                        ? "first available model"
                        : config.wakeModelKeyword);
      return false;
    }
    const int sampleRate = wakeNet->get_samp_rate(wakeNetData);
    const int chunkSamples = wakeNet->get_samp_chunksize(wakeNetData);
    const int channels = wakeNet->get_channel_num == nullptr
                             ? 0
                             : wakeNet->get_channel_num(wakeNetData);
    if (sampleRate != static_cast<int>(captureFormat.sample_rate) ||
        chunkSamples <= 0 || channels != 1) {
      Serial.printf("[wake] unsupported model format: rate=%d chunk=%d channels=%d\n",
                    sampleRate, chunkSamples, channels);
      return false;
    }
    wakeChunkSamples = static_cast<size_t>(chunkSamples);
    wakeDetectChunk.resize(wakeChunkSamples);
    wakePcm.clear();
    wakePcm.reserve(wakeChunkSamples + wakeMono.size());
    wakeTaskRunning.store(true);
    wakeTaskExited.store(false);
    if (xTaskCreatePinnedToCore(wakeTaskEntry, "wakenet", config.wakeTaskStackBytes,
                                this, config.wakeTaskPriority, &wakeTask,
                                config.wakeTaskCore) != pdPASS) {
      wakeTaskRunning.store(false);
      wakeTaskExited.store(true);
      Serial.println("[wake] failed to create WakeNet task");
      return false;
    }
    wakeDetectionStarted = true;
    Serial.printf("[wake] WakeNet ready: model=%s rate=%d chunk=%d\n",
                  modelName, sampleRate, chunkSamples);
    return true;
  }

  void setWakeDetection(bool enabled) {
    if (!wakeDetectionStarted) {
      return;
    }
    bool changed = false;
    portENTER_CRITICAL(&wakeStateMux);
    if (wakeDetectionEnabled.load() != enabled ||
        (!enabled && wakeDetected.load())) {
      wakeGeneration.fetch_add(1);
      wakeDetected.store(false);
      wakeDetectedGeneration.store(0);
      if (enabled) {
        wakeResetRequested.store(true);
      }
      wakeDetectionEnabled.store(enabled);
      changed = true;
    }
    portEXIT_CRITICAL(&wakeStateMux);
    if (!changed) {
      return;
    }
    if (enabled) {
      if (wakeTask != nullptr) {
        xTaskNotifyGive(wakeTask);
      }
      if (Serial) {
        Serial.printf("[wake] listening for: %s\n", config.defaultWakeWord);
      }
      return;
    }
    if (Serial) {
      Serial.println("[wake] paused");
    }
  }

  bool consumeWake(std::string& wakeWord) {
    std::array<char, sizeof(lastWakeWord)> detectedWord{};
    bool consumed = false;
    portENTER_CRITICAL(&wakeStateMux);
    if (wakeDetected.load() &&
        wakeDetectedGeneration.load() == wakeGeneration.load()) {
      std::copy_n(lastWakeWord, detectedWord.size(), detectedWord.data());
      consumed = true;
    }
    wakeDetected.store(false);
    wakeDetectedGeneration.store(0);
    portEXIT_CRITICAL(&wakeStateMux);
    if (!consumed) {
      return false;
    }
    // String assignment may allocate; keep it outside the interrupt-disabled
    // wake-state critical section.
    wakeWord = detectedWord[0] == '\0' ? config.defaultWakeWord
                                        : detectedWord.data();
    return true;
  }

  void stopWakeDetection() {
    if (!wakeDetectionStarted && wakeTask == nullptr && wakeNetData == nullptr &&
        wakeModels == nullptr) {
      return;
    }
    portENTER_CRITICAL(&wakeStateMux);
    wakeDetectionEnabled.store(false);
    wakeGeneration.fetch_add(1);
    wakeDetected.store(false);
    wakeDetectedGeneration.store(0);
    wakeTaskRunning.store(false);
    portEXIT_CRITICAL(&wakeStateMux);
    if (wakeTask != nullptr) {
      xTaskNotifyGive(wakeTask);
    }
    for (int attempt = 0; attempt < 100 && !wakeTaskExited.load(); ++attempt) {
      delay(10);
    }
    if (wakeTask != nullptr) {
      vTaskDelete(wakeTask);
    }
    wakeTask = nullptr;
    wakeTaskExited.store(true);
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
#else
  bool beginWakeDetection() {
    if (config.enableWakeDetection) {
      Serial.println("[wake] ESP-SR wake backend was not compiled");
      return false;
    }
    return true;
  }

  void setWakeDetection(bool) {}

  bool consumeWake(std::string&) { return false; }

  void stopWakeDetection() {}
#endif

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
      ++decodeErrors;
      return false;
    }
    decoderRate = rate;
    decoderDurationMs = durationMs;
    return true;
  }

  static void decoderTaskEntry(void* context) {
    static_cast<Impl*>(context)->decoderLoop();
  }

  static void inputTaskEntry(void* context) {
    static_cast<Impl*>(context)->inputLoop();
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
    workersRunning.store(true);
    outputTaskExited.store(false);
    if (xTaskCreate(outputTaskEntry, "audio_output", config.outputTaskStackBytes,
                    this, config.outputTaskPriority, &outputTask) != pdPASS) {
      outputTaskExited.store(true);
      Serial.println("[audio] failed to create output task");
      stopWorkers();
      return false;
    }
    decoderTaskExited.store(false);
    if (xTaskCreate(decoderTaskEntry, "opus_codec", config.decoderTaskStackBytes,
                    this, config.decoderTaskPriority, &decoderTask) != pdPASS) {
      decoderTaskExited.store(true);
      Serial.println("[audio] failed to create Opus codec task");
      stopWorkers();
      return false;
    }
    inputTaskExited.store(false);
    if (xTaskCreate(inputTaskEntry, "audio_input", config.inputTaskStackBytes,
                    this, config.inputTaskPriority, &inputTask) != pdPASS) {
      inputTaskExited.store(true);
      Serial.println("[audio] failed to create input task");
      stopWorkers();
      return false;
    }
    return true;
  }

  static void releaseVector(std::vector<int16_t>& buffer) {
    std::vector<int16_t>().swap(buffer);
  }

  static void releaseVector(std::vector<int32_t>& buffer) {
    std::vector<int32_t>().swap(buffer);
  }

  void releaseWorkerBuffers() {
    encodeQueue.release();
    uplinkQueue.release();
    decodeQueue.release();
    playbackQueue.release();
    std::vector<std::vector<int16_t>>().swap(capturePool);
    std::vector<std::vector<uint8_t>>().swap(uplinkPool);
    std::vector<std::vector<int16_t>>().swap(playbackPool);
    decodeInFlight = false;
    outputInFlight = false;
  }

  void stopWorkers() {
    if (queueMutex == nullptr) {
      // initializeBufferPools() runs before task creation. Release those
      // allocations even when mutex creation itself failed under low memory.
      releaseWorkerBuffers();
      return;
    }
    workersRunning.store(false);
    captureEnabled.store(false);
    if (inputTask != nullptr) {
      xTaskNotifyGive(inputTask);
    }
    if (decoderTask != nullptr) {
      xTaskNotifyGive(decoderTask);
    }
    if (outputTask != nullptr) {
      xTaskNotifyGive(outputTask);
    }
    for (int attempt = 0; attempt < 100 &&
         (!inputTaskExited.load() || !decoderTaskExited.load() ||
          !outputTaskExited.load()); ++attempt) {
      delay(10);
    }
    if (inputTask != nullptr) {
      vTaskDelete(inputTask);
    }
    if (decoderTask != nullptr) {
      vTaskDelete(decoderTask);
    }
    if (outputTask != nullptr) {
      vTaskDelete(outputTask);
    }
    inputTask = nullptr;
    decoderTask = nullptr;
    outputTask = nullptr;
    inputTaskExited.store(true);
    decoderTaskExited.store(true);
    outputTaskExited.store(true);
    // All worker tasks have exited or were force-deleted, so no lock is needed
    // while releasing their queues and nested buffer capacities.
    releaseWorkerBuffers();
    vSemaphoreDelete(queueMutex);
    queueMutex = nullptr;
  }

  void inputLoop() {
    while (workersRunning.load()) {
      if (!captureEnabled.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        continue;
      }
      // Snapshot the generation before consuming the reset request. If a
      // disable->enable transition races this iteration, pumpCapture() keeps
      // the old generation and discards the read instead of labelling old
      // buffered samples as belonging to the new session.
      const uint32_t generation = captureGeneration.load();
      if (captureResetRequested.exchange(false)) {
        capturedSamples = 0;
        speechGateOpen = false;
        speechStartPackets = 0;
        speechHoldPackets = 0;
        captureResampler.initialized = false;
      }
      pumpCapture(generation);
    }
    inputTaskExited.store(true);
    vTaskSuspend(nullptr);
  }

  void decoderLoop() {
    while (workersRunning.load()) {
      CaptureChunk capture;
      DecodePacket packet;
      PlaybackChunk chunk;
      bool haveCapture = false;
      bool haveFrame = false;
      if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        if (workersRunning.load() && !encodeQueue.empty()) {
          capture = std::move(encodeQueue.front());
          encodeQueue.pop_front();
          haveCapture = true;
        }
        if (workersRunning.load() && !decodeQueue.empty() &&
            playbackQueue.size() < config.maximumPlaybackChunks &&
            !playbackPool.empty()) {
          packet = std::move(decodeQueue.front());
          decodeQueue.pop_front();
          chunk.pcm = std::move(playbackPool.back());
          playbackPool.pop_back();
          chunk.generation = packet.generation;
          decodeInFlight = true;
          haveFrame = true;
        }
        xSemaphoreGive(queueMutex);
      }
      if (!workersRunning.load()) {
        break;
      }
      if (!haveCapture && !haveFrame) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        continue;
      }

      // Match the official AudioService policy: decoded speech has priority.
      // This minimizes the time to first audible packet in realtime/AEC modes;
      // the measured encode+decode cost remains comfortably below one frame.
      if (haveFrame) {
        if (packet.resetDecoder) {
          decoderResetRequested.store(true);
        }
        const uint32_t decodeStartedUs = micros();
        const bool decoded =
            packet.generation == playbackGeneration.load() &&
            !playbackSuppressed.load() && decodeFrame(packet.frame, chunk);
        updateMaximum(maximumDecodeUs, micros() - decodeStartedUs);
        if (decoded &&
            (chunk.generation != playbackGeneration.load() ||
             playbackSuppressed.load())) {
          // cancelPlayback() may race after decodeFrame consumed its reset
          // request. The stale PCM is discarded below; re-arm the reset so the
          // next generation cannot inherit prediction state from this frame.
          decoderResetRequested.store(true);
        }
        if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
          if (decoded && workersRunning.load() &&
              chunk.generation == playbackGeneration.load() &&
              !playbackSuppressed.load()) {
            if (!playbackQueue.push_back(std::move(chunk))) {
              playbackPool.push_back(std::move(chunk.pcm));
              decoderResetRequested.store(true);
              ++droppedDecodePackets;
            }
          } else {
            playbackPool.push_back(std::move(chunk.pcm));
          }
          decodeInFlight = false;
          xSemaphoreGive(queueMutex);
        }
        if (outputTask != nullptr) {
          xTaskNotifyGive(outputTask);
        }
      }

      if (haveCapture) {
        encodeCapturedPacket(capture);
        if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
          capturePool.push_back(std::move(capture.pcm));
          xSemaphoreGive(queueMutex);
        }
      }
    }
    decoderTaskExited.store(true);
    vTaskSuspend(nullptr);
  }

  void outputLoop() {
    while (workersRunning.load()) {
      PlaybackChunk chunk;
      bool haveChunk = false;
      if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        if (workersRunning.load() && !playbackQueue.empty()) {
          chunk = std::move(playbackQueue.front());
          playbackQueue.pop_front();
          outputInFlight = true;
          haveChunk = true;
        }
        xSemaphoreGive(queueMutex);
      }
      if (!workersRunning.load()) {
        break;
      }
      if (haveChunk) {
        const bool played = writePlaybackChunk(chunk);
        if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
          // cancelPlayback() invalidates the generation before taking this
          // mutex. Recheck while committing so an old, just-written chunk
          // cannot repopulate the AEC timestamp FIFO after it was cleared.
          if (played && workersRunning.load() &&
              chunk.generation == playbackGeneration.load() &&
              !playbackSuppressed.load()) {
            pushPlaybackTimestampLocked(chunk.timestamp, chunk.generation);
          }
          outputInFlight = false;
          playbackPool.push_back(std::move(chunk.pcm));
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
      if (drained && !playbackMuted.load() &&
          millis() - lastPlaybackWriteMs.load() > config.playbackMuteDelayMs) {
        applyCodecMute(true);
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
    outputTaskExited.store(true);
    vTaskSuspend(nullptr);
  }

  void stop() {
    captureEnabled.store(false);
    captureGeneration.fetch_add(1);
    if (queueMutex != nullptr) {
      cancelPlayback();
    }
    stopWakeDetection();
    stopWorkers();
    uplink = {};
    if (codecMutex != nullptr) {
      applyCodecMute(true);
    }
    if (encoder != nullptr) {
      esp_opus_enc_close(encoder);
      encoder = nullptr;
    }
    if (decoder != nullptr) {
      esp_opus_dec_close(decoder);
      decoder = nullptr;
    }
    decoderRate = 0;
    decoderDurationMs = 0;
    if (externalCodecStarted) {
      config.codec.end(config.codec.context);
      externalCodecStarted = false;
    }
#if XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
    else if (codec != nullptr) {
      codec->enable(codec, false);
      codec->close(codec);
      free(const_cast<audio_codec_if_t*>(codec));
      codec = nullptr;
    }
#endif
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
    if (codecMutex != nullptr) {
      vSemaphoreDelete(codecMutex);
      codecMutex = nullptr;
    }
    releaseVector(captured);
    releaseVector(readBuffer);
    releaseVector(readBuffer32);
    releaseVector(writeBuffer32);
    releaseVector(decodeScratch);
    releaseVector(wakeCaptured);
    releaseVector(wakeMono);
    releaseVector(wakeDetectChunk);
    releaseVector(wakePcm);
    capturedSamples = 0;
    started = false;
  }

  void resampleCapture(const int16_t* interleaved, size_t sourceFrames,
                       std::vector<int16_t>& output,
                       CaptureResamplerState& resampler, char& selectedChannel,
                       uint32_t& peak, uint32_t& rms,
                       bool collectMetrics = true) {
    size_t channel = 0;
    if (inputChannels == 2) {
      if (config.captureChannel == CaptureChannel::Right) {
        channel = 1;
      } else if (config.captureChannel == CaptureChannel::Auto) {
        uint64_t leftEnergy = 0;
        uint64_t rightEnergy = 0;
        for (size_t frame = 0; frame < sourceFrames; ++frame) {
          const int32_t left = interleaved[frame * inputChannels];
          const int32_t right = interleaved[frame * inputChannels + 1];
          leftEnergy += static_cast<uint64_t>(left * left);
          rightEnergy += static_cast<uint64_t>(right * right);
        }
        if (static_cast<double>(rightEnergy) >
            static_cast<double>(leftEnergy) * config.autoChannelSwitchRatio) {
          channel = 1;
        }
      }
    }
    selectedChannel = channel == 0 ? 'L' : 'R';
    if (!resampler.initialized || resampler.channel != selectedChannel) {
      resampler.history.fill(interleaved[channel]);
      resampler.channel = selectedChannel;
      resampler.initialized = true;
    }

    // The 15-tap Hamming-windowed low-pass is generated once for the configured
    // rate ratio. Q15 arithmetic keeps both capture and WakeNet hot paths cheap.
    const bool filter = config.captureAntiAlias &&
                        inputSampleRate() > captureFormat.sample_rate;
    auto filteredSample = [&](size_t sourceIndex) -> int16_t {
      if (!filter) {
        return interleaved[sourceIndex * inputChannels + channel];
      }
      int64_t accumulator = 0;
      for (size_t tap = 0;
           tap < captureAntiAliasQ15.size(); ++tap) {
        const int64_t sampleIndex = static_cast<int64_t>(sourceIndex) -
                                    static_cast<int64_t>(tap);
        const int16_t sample = sampleIndex >= 0
                                   ? interleaved[static_cast<size_t>(sampleIndex) *
                                                     inputChannels +
                                                 channel]
                                   : resampler.history[static_cast<size_t>(
                                         static_cast<int64_t>(resampler.history.size()) +
                                         sampleIndex)];
        accumulator += static_cast<int32_t>(sample) * captureAntiAliasQ15[tap];
      }
      const int32_t rounded = static_cast<int32_t>((accumulator + (1 << 14)) >> 15);
      return static_cast<int16_t>(
          std::max<int32_t>(-32768, std::min<int32_t>(32767, rounded)));
    };
    uint64_t outputEnergy = 0;
    peak = 0;
    const uint64_t phaseStep =
        (static_cast<uint64_t>(inputSampleRate()) << 32) /
        captureFormat.sample_rate;
    uint64_t phase = 0;
    size_t cachedSourceIndex = sourceFrames;
    int16_t cachedSample = 0;
    const auto sampleAt = [&](size_t sourceIndex) -> int16_t {
      if (filter && sourceIndex == cachedSourceIndex) {
        return cachedSample;
      }
      const int16_t sample = filteredSample(sourceIndex);
      if (filter) {
        cachedSourceIndex = sourceIndex;
        cachedSample = sample;
      }
      return sample;
    };
    const bool upsample = inputSampleRate() < captureFormat.sample_rate;
    for (size_t index = 0; index < output.size(); ++index, phase += phaseStep) {
      const size_t current = std::min<size_t>(phase >> 32, sourceFrames - 1);
      const uint32_t fraction = static_cast<uint32_t>(phase);
      int32_t sample = sampleAt(current);
      if (upsample) {
        // Forward interpolation needs the first sample of the next I2S block
        // for the final fractional output. Use a one-input-sample causal delay
        // instead, interpolating previous -> current across every boundary.
        const int32_t previous = current == 0
                                     ? resampler.history.back()
                                     : sampleAt(current - 1);
        sample = previous + static_cast<int32_t>(
            (static_cast<int64_t>(sample - previous) * fraction) >> 32);
      } else {
        const size_t next = std::min(current + 1, sourceFrames - 1);
        const int32_t nextSample = sampleAt(next);
        sample += static_cast<int32_t>(
            (static_cast<int64_t>(nextSample - sample) * fraction) >> 32);
      }
      output[index] = static_cast<int16_t>(sample);
      if (collectMetrics) {
        const uint32_t magnitude = static_cast<uint32_t>(std::abs(sample));
        peak = std::max(peak, magnitude);
        outputEnergy += static_cast<uint64_t>(sample * sample);
      }
    }
    if (sourceFrames >= resampler.history.size()) {
      for (size_t index = 0; index < resampler.history.size(); ++index) {
        resampler.history[index] =
            interleaved[(sourceFrames - resampler.history.size() + index) *
                            inputChannels +
                        channel];
      }
    } else if (sourceFrames != 0) {
      // Small but valid readFramesPerLoop values must advance the FIR history
      // too. Otherwise every block boundary keeps referring to the very first
      // sample captured after reset.
      std::move(resampler.history.begin() + sourceFrames,
                resampler.history.end(), resampler.history.begin());
      for (size_t index = 0; index < sourceFrames; ++index) {
        resampler.history[resampler.history.size() - sourceFrames + index] =
            interleaved[index * inputChannels + channel];
      }
    }
    rms = collectMetrics
              ? static_cast<uint32_t>(std::sqrt(
                    static_cast<double>(outputEnergy) /
                    static_cast<double>(output.size())))
              : 0;
  }

  float conditionSpeech(std::vector<int16_t>& pcm, uint32_t rawRms,
                        uint32_t& peak, uint32_t& rms) {
    if (!config.enableSpeechConditioning) {
      return 1.0f;
    }

    const uint16_t holdPackets = std::max<uint16_t>(
        1, (config.speechHoldMs + captureFormat.frame_duration_ms - 1) /
               captureFormat.frame_duration_ms);
    if (speechGateOpen) {
      if (rawRms >= config.speechGateRms) {
        speechHoldPackets = holdPackets;
      } else if (speechHoldPackets > 0 && --speechHoldPackets == 0) {
        speechGateOpen = false;
        speechStartPackets = 0;
      }
    } else if (rawRms >= config.speechGateRms) {
      speechStartPackets = std::min<uint8_t>(
          config.speechStartPackets, speechStartPackets + 1);
      if (speechStartPackets >= config.speechStartPackets) {
        speechGateOpen = true;
        speechHoldPackets = holdPackets;
      }
    } else {
      speechStartPackets = 0;
    }

    // Preserve the first candidate packet at unity gain. If the next packet
    // also contains speech the gate opens, while an isolated click is never
    // amplified and cannot keep server-side VAD active.
    float gain = speechStartPackets > 0 ? 1.0f : config.speechSilenceGain;
    if (speechGateOpen) {
      gain = std::min(config.speechMaximumGain,
                      static_cast<float>(config.speechTargetRms) /
                          static_cast<float>(std::max<uint32_t>(rawRms, 1)));
      gain = std::max(1.0f, gain);
    }

    uint64_t conditionedEnergy = 0;
    peak = 0;
    for (int16_t& input : pcm) {
      const int32_t scaled = static_cast<int32_t>(std::lround(input * gain));
      const int16_t sample = static_cast<int16_t>(
          std::max<int32_t>(-32768, std::min<int32_t>(32767, scaled)));
      input = sample;
      const uint32_t magnitude = static_cast<uint32_t>(std::abs(sample));
      peak = std::max(peak, magnitude);
      conditionedEnergy += static_cast<uint64_t>(sample * sample);
    }
    rms = static_cast<uint32_t>(std::sqrt(
        static_cast<double>(conditionedEnergy) / static_cast<double>(pcm.size())));
    return gain;
  }

  void queueCapturedPacket(uint32_t generation) {
    CaptureChunk chunk;
    chunk.generation = generation;
    // Claim a pooled PCM buffer under the lock, then do all DSP outside it.
    // The input task is the sole producer, so the reserved queue slot cannot
    // refill while this packet is being prepared.
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
      if (!workersRunning.load() || !captureEnabled.load() ||
          generation != captureGeneration.load()) {
        xSemaphoreGive(queueMutex);
        return;
      }
      if (encodeQueue.size() >= config.maximumEncodePackets) {
        chunk.pcm = std::move(encodeQueue.front().pcm);
        encodeQueue.pop_front();
        ++droppedEncodePackets;
      } else if (!capturePool.empty()) {
        chunk.pcm = std::move(capturePool.back());
        capturePool.pop_back();
      } else {
        ++droppedEncodePackets;
        xSemaphoreGive(queueMutex);
        return;
      }
      xSemaphoreGive(queueMutex);
    }

    chunk.pcm.resize(captureSamplesPerPacket);
    resampleCapture(captured.data(), hardwareFramesPerPacket, chunk.pcm,
                    captureResampler, chunk.channel, chunk.peak, chunk.rms);
    chunk.rawRms = chunk.rms;
    chunk.gain = conditionSpeech(chunk.pcm, chunk.rawRms, chunk.peak, chunk.rms);
    chunk.gateOpen = speechGateOpen;

    bool queued = false;
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
      if (workersRunning.load() && captureEnabled.load() &&
          generation == captureGeneration.load()) {
        // Server-side AEC expects the timestamp of a downlink packet that has
        // actually reached I2S, not the microphone's local millis() value.
        const PlaybackTimestamp playbackTimestamp = popPlaybackTimestampLocked();
        chunk.timestamp = playbackTimestamp.value;
        chunk.timestampGeneration = playbackTimestamp.generation;
        if (encodeQueue.push_back(std::move(chunk))) {
          queued = true;
        } else {
          capturePool.push_back(std::move(chunk.pcm));
          ++droppedEncodePackets;
        }
      } else {
        capturePool.push_back(std::move(chunk.pcm));
        ++discardedStaleCapturePackets;
      }
      xSemaphoreGive(queueMutex);
    }
    if (queued && decoderTask != nullptr) {
      xTaskNotifyGive(decoderTask);
    }
  }

  void encodeCapturedPacket(CaptureChunk& capture) {
    if (!captureEnabled.load() ||
        capture.generation != captureGeneration.load()) {
      ++discardedStaleCapturePackets;
      return;
    }
    UplinkPacket packet;
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
      if (!uplinkPool.empty()) {
        packet.opus = std::move(uplinkPool.back());
        uplinkPool.pop_back();
      } else if (!uplinkQueue.empty()) {
        packet.opus = std::move(uplinkQueue.front().opus);
        uplinkQueue.pop_front();
        ++droppedUplinkPackets;
      }
      xSemaphoreGive(queueMutex);
    }
    packet.opus.resize(static_cast<size_t>(encoderOutputBytes));
    packet.timestamp = capture.timestamp;
    packet.timestampGeneration = capture.timestampGeneration;
    packet.generation = capture.generation;

    esp_audio_enc_in_frame_t input{};
    input.buffer = reinterpret_cast<uint8_t*>(capture.pcm.data());
    input.len = static_cast<uint32_t>(capture.pcm.size() * sizeof(int16_t));
    esp_audio_enc_out_frame_t output{};
    output.buffer = packet.opus.data();
    output.len = static_cast<uint32_t>(packet.opus.size());
    const uint32_t encodeStartedUs = micros();
    const esp_audio_err_t result = esp_opus_enc_process(encoder, &input, &output);
    updateMaximum(maximumEncodeUs, micros() - encodeStartedUs);
    if (result != ESP_AUDIO_ERR_OK || output.encoded_bytes == 0) {
      ++encodeErrors;
      if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        uplinkPool.push_back(std::move(packet.opus));
        xSemaphoreGive(queueMutex);
      }
      return;
    }
    packet.opus.resize(output.encoded_bytes);
    bool queued = false;
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
      // This commit check must stay inside the same critical section as the
      // push. disable->clear can no longer be followed by a stale reinsert.
      if (workersRunning.load() && captureEnabled.load() &&
          packet.generation == captureGeneration.load()) {
        if (uplinkQueue.size() >= config.maximumUplinkPackets) {
          uplinkPool.push_back(std::move(uplinkQueue.front().opus));
          uplinkQueue.pop_front();
          ++droppedUplinkPackets;
        }
        if (uplinkQueue.push_back(std::move(packet))) {
          queued = true;
        } else {
          uplinkPool.push_back(std::move(packet.opus));
          ++droppedUplinkPackets;
        }
      } else {
        uplinkPool.push_back(std::move(packet.opus));
        ++discardedStaleCapturePackets;
      }
      xSemaphoreGive(queueMutex);
    }
    if (!queued) {
      return;
    }
    const uint32_t packetNumber = ++capturedPackets;
    if (packetNumber == 1 ||
        packetNumber % config.captureLogIntervalPackets == 0) {
      // Realtime codec tasks must never print directly: USB CDC output can
      // block while no monitor is attached and make the input queue overrun.
      // Publish a cheap snapshot for the main-loop performance report instead.
      latestCapturePeak.store(capture.peak);
      latestCaptureRms.store(capture.rms);
      latestCaptureRawRms.store(capture.rawRms);
      latestCaptureOpusBytes.store(output.encoded_bytes);
    }
  }

  static int16_t pcm32To16(int32_t value, uint8_t rightShift) {
    const int32_t shifted = rightShift == 0
                                ? value
                                : (rightShift >= 31 ? (value < 0 ? -1 : 0)
                                                    : value >> rightShift);
    if (shifted > INT16_MAX) {
      return INT16_MAX;
    }
    if (shifted < INT16_MIN) {
      return INT16_MIN;
    }
    return static_cast<int16_t>(shifted);
  }

  esp_err_t readCaptureSamples(int16_t* destination, size_t sampleCapacity,
                               size_t& samplesRead, TickType_t timeout) {
    samplesRead = 0;
    const StandardI2sEndpoint& input = config.hardware.input;
    if (config.hardware.inputMode == InputMode::I2sPdm ||
        input.dataBits == 16) {
      size_t bytesRead = 0;
      const esp_err_t result = i2s_channel_read(
          rx, destination, sampleCapacity * sizeof(int16_t), &bytesRead, timeout);
      samplesRead = bytesRead / sizeof(int16_t);
      return result;
    }

    size_t bytesRead = 0;
    const esp_err_t result = i2s_channel_read(
        rx, readBuffer32.data(), sampleCapacity * sizeof(int32_t), &bytesRead,
        timeout);
    const size_t rawSamples = bytesRead / sizeof(int32_t);
    for (size_t index = 0; index < rawSamples; ++index) {
      destination[index] = pcm32To16(readBuffer32[index], input.rightShift);
    }
    samplesRead = rawSamples;
    return result;
  }

  void pumpCapture(uint32_t generation) {
    if (!captureEnabled.load() || rxMutex == nullptr ||
        xSemaphoreTake(rxMutex, pdMS_TO_TICKS(25)) != pdTRUE) {
      return;
    }
    size_t samplesRead = 0;
    const esp_err_t result = readCaptureSamples(
        readBuffer.data(), readBuffer.size(), samplesRead, pdMS_TO_TICKS(20));
    xSemaphoreGive(rxMutex);
    if (!captureEnabled.load() || generation != captureGeneration.load()) {
      return;
    }
    if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
      ++inputErrors;
      vTaskDelay(pdMS_TO_TICKS(5));
      return;
    }
    size_t sourceOffset = 0;
    while (sourceOffset < samplesRead && captureEnabled.load() &&
           generation == captureGeneration.load()) {
      const size_t available = captured.size() - capturedSamples;
      const size_t copying = std::min(available, samplesRead - sourceOffset);
      std::copy_n(readBuffer.data() + sourceOffset, copying,
                  captured.data() + capturedSamples);
      capturedSamples += copying;
      sourceOffset += copying;
      if (capturedSamples == captured.size()) {
        queueCapturedPacket(generation);
        capturedSamples = 0;
      }
    }
  }

  bool decodeFrame(const xiaozhi::AudioFrame& frame, PlaybackChunk& chunk) {
    const uint32_t sourceRate = frame.format.sample_rate;
    const uint16_t durationMs = frame.format.frame_duration_ms;
    if (sourceRate == 0 || durationMs == 0 || frame.opus.empty()) {
      return false;
    }

    const size_t maximumFrames = sourceRate * durationMs / 1000 + 16;
    if (decoderResetRequested.exchange(false)) {
      if (decoder != nullptr) {
        esp_opus_dec_close(decoder);
        decoder = nullptr;
      }
      decoderRate = 0;
      decoderDurationMs = 0;
      playbackResampler.initialized = false;
      playbackResampler.interpolationInitialized = false;
    }
    if (maximumFrames > decodeScratch.size() ||
        !ensureDecoder(sourceRate, durationMs)) {
      return false;
    }
    esp_audio_dec_in_raw_t input{};
    input.buffer = const_cast<uint8_t*>(frame.opus.data());
    input.len = static_cast<uint32_t>(frame.opus.size());
    esp_audio_dec_out_frame_t output{};
    output.buffer = reinterpret_cast<uint8_t*>(decodeScratch.data());
    output.len = static_cast<uint32_t>(decodeScratch.size() * sizeof(int16_t));
    esp_audio_dec_info_t info{};
    const esp_audio_err_t result = esp_opus_dec_decode(decoder, &input, &output, &info);
    if (result != ESP_AUDIO_ERR_OK || output.decoded_size == 0 || info.channel != 1) {
      ++decodeErrors;
      decoderResetRequested.store(true);
      return false;
    }

    const size_t decodedSamples = output.decoded_size / sizeof(int16_t);
    const size_t sourceFrames = decodedSamples / info.channel;
    if (sourceFrames == 0) {
      return false;
    }
    if (info.sample_rate == 0) {
      ++decodeErrors;
      return false;
    }
    const size_t destinationFrames = static_cast<size_t>(
        (static_cast<uint64_t>(sourceFrames) * outputSampleRate() +
         info.sample_rate / 2) /
        info.sample_rate);
    chunk.pcm.resize(destinationFrames * outputChannels);
    chunk.sourceRate = info.sample_rate;
    chunk.timestamp = frame.timestamp;
    chunk.durationMs = durationMs;
    chunk.sourceChannels = info.channel;
    const bool sameRate = info.sample_rate == outputSampleRate();
    const bool filterPlayback = config.playbackAntiAlias &&
                                info.sample_rate > outputSampleRate();
    const bool sourceRateChanged =
        playbackResampler.sourceRate != info.sample_rate;
    if (sourceRateChanged) {
      playbackResampler.sourceRate = info.sample_rate;
      playbackResampler.initialized = false;
      playbackResampler.interpolationInitialized = false;
    }
    if (filterPlayback && !playbackResampler.initialized) {
      initializeAntiAliasFilter(info.sample_rate, outputSampleRate(),
                                playbackAntiAliasQ15);
      playbackResampler.history.fill(decodeScratch[0]);
      playbackResampler.initialized = true;
    } else if (!filterPlayback) {
      // Force a fresh history if a later session switches back to a rate that
      // requires downsampling.
      playbackResampler.initialized = false;
    }
    const bool upsample = info.sample_rate < outputSampleRate();
    if (upsample && !playbackResampler.interpolationInitialized) {
      playbackResampler.previousSample = decodeScratch[0];
      playbackResampler.interpolationInitialized = true;
    } else if (!upsample) {
      playbackResampler.interpolationInitialized = false;
    }
    auto filteredPlaybackSample = [&](size_t sourceIndex) -> int16_t {
      if (!filterPlayback) {
        return decodeScratch[sourceIndex];
      }
      int64_t accumulator = 0;
      for (size_t tap = 0; tap < playbackAntiAliasQ15.size(); ++tap) {
        const int64_t sampleIndex = static_cast<int64_t>(sourceIndex) -
                                    static_cast<int64_t>(tap);
        const int16_t sample = sampleIndex >= 0
                                   ? decodeScratch[static_cast<size_t>(sampleIndex)]
                                   : playbackResampler.history[static_cast<size_t>(
                                         static_cast<int64_t>(
                                             playbackResampler.history.size()) +
                                         sampleIndex)];
        accumulator += static_cast<int32_t>(sample) *
                       playbackAntiAliasQ15[tap];
      }
      const int32_t rounded =
          static_cast<int32_t>((accumulator + (1 << 14)) >> 15);
      return static_cast<int16_t>(
          std::max<int32_t>(-32768, std::min<int32_t>(32767, rounded)));
    };
    const uint64_t phaseStep = sameRate
                                   ? (uint64_t{1} << 32)
                                   : (static_cast<uint64_t>(info.sample_rate) << 32) /
                                         outputSampleRate();
    uint64_t phase = 0;
    for (size_t index = 0; index < destinationFrames; ++index, phase += phaseStep) {
      const size_t current = std::min<size_t>(phase >> 32, sourceFrames - 1);
      int32_t sample = filteredPlaybackSample(current);
      const uint32_t fraction = static_cast<uint32_t>(phase);
      if (upsample) {
        const int32_t previous = current == 0
                                     ? playbackResampler.previousSample
                                     : filteredPlaybackSample(current - 1);
        sample = previous + static_cast<int32_t>(
            (static_cast<int64_t>(sample - previous) * fraction) >> 32);
      } else if (!sameRate && fraction != 0) {
        const size_t next = std::min(current + 1, sourceFrames - 1);
        const int32_t nextSample = filteredPlaybackSample(next);
        sample += static_cast<int32_t>(
            (static_cast<int64_t>(nextSample - sample) * fraction) >> 32);
      }
      for (size_t channel = 0; channel < outputChannels; ++channel) {
        chunk.pcm[index * outputChannels + channel] =
            static_cast<int16_t>(sample);
      }
    }
    if (upsample) {
      playbackResampler.previousSample = decodeScratch[sourceFrames - 1];
    }
    if (filterPlayback) {
      if (sourceFrames >= playbackResampler.history.size()) {
        std::copy_n(decodeScratch.data() + sourceFrames -
                                             playbackResampler.history.size(),
                    playbackResampler.history.size(),
                    playbackResampler.history.begin());
      } else {
        std::move(playbackResampler.history.begin() + sourceFrames,
                  playbackResampler.history.end(),
                  playbackResampler.history.begin());
        std::copy_n(decodeScratch.data(), sourceFrames,
                    playbackResampler.history.end() - sourceFrames);
      }
    }
    return true;
  }

  bool writePlaybackChunk(const PlaybackChunk& chunk) {
    if (chunk.generation != playbackGeneration.load() || playbackSuppressed.load()) {
      return false;
    }
    if (playbackMuted.load()) {
      if (!applyCodecMute(false)) {
        ++outputErrors;
        return false;
      }
    }

    // This runs only on the dedicated audio_output task, matching the original
    // xiaozhi-esp32 AudioService. The network task never blocks on I2S.
    const uint8_t* pcm = nullptr;
    size_t totalBytes = 0;
    if (config.hardware.output.dataBits == 32) {
      writeBuffer32.resize(chunk.pcm.size());
      // Decoded Opus is always PCM16. The ESP-IDF 32-bit DMA word must carry
      // that sample in its most-significant 16 bits, matching the official
      // NoAudioCodec path and avoiding a 48/96 dB low-bit attenuation.
      constexpr uint8_t leftShift = 16;
      const int32_t outputGainQ15 =
          config.hardware.codecMode == CodecMode::None ? speakerGainQ15 : 32768;
      for (size_t index = 0; index < chunk.pcm.size(); ++index) {
        const int32_t scaled = static_cast<int32_t>(
            (static_cast<int64_t>(chunk.pcm[index]) * outputGainQ15) >> 15);
        writeBuffer32[index] = leftShift == 0
                                   ? scaled
                                   : static_cast<int32_t>(
                                         static_cast<int64_t>(scaled) *
                                         (int64_t{1} << leftShift));
      }
      pcm = reinterpret_cast<const uint8_t*>(writeBuffer32.data());
      totalBytes = writeBuffer32.size() * sizeof(int32_t);
    } else {
      pcm = reinterpret_cast<const uint8_t*>(chunk.pcm.data());
      totalBytes = chunk.pcm.size() * sizeof(int16_t);
    }
    size_t offset = 0;
    while (offset < totalBytes) {
      if (chunk.generation != playbackGeneration.load() || playbackSuppressed.load()) {
        applyCodecMute(true);
        return false;
      }
      size_t written = 0;
      const esp_err_t writeResult =
          i2s_channel_write(tx, pcm + offset, totalBytes - offset, &written,
                            pdMS_TO_TICKS(config.playbackWriteTimeoutMs));
      if (writeResult != ESP_OK || written == 0) {
        ++outputErrors;
        applyCodecMute(true);
        return false;
      }
      offset += written;
      lastPlaybackWriteMs.store(millis());
    }
    ++playedPackets;
    return true;
  }

  void enqueueDecode(xiaozhi::AudioFrame frame) {
    if (!workersRunning.load() || queueMutex == nullptr) {
      return;
    }
    bool queued = false;
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
      uint32_t queuedMs = 0;
      for (const auto& packet : decodeQueue) {
        queuedMs += packet.frame.format.frame_duration_ms;
      }
      bool droppedForLatency = false;
      if (decodeQueue.size() >= decodeQueue.capacity()) {
        queuedMs -= decodeQueue.front().frame.format.frame_duration_ms;
        decodeQueue.pop_front();
        droppedForLatency = true;
        ++droppedDecodePackets;
      }
      while (!decodeQueue.empty() &&
             queuedMs + frame.format.frame_duration_ms >
                 config.maximumDecodeQueueMs) {
        queuedMs -= decodeQueue.front().frame.format.frame_duration_ms;
        decodeQueue.pop_front();
        droppedForLatency = true;
        ++droppedDecodePackets;
      }
      playbackSuppressed.store(false);
      DecodePacket packet;
      packet.frame = std::move(frame);
      packet.generation = playbackGeneration.load();
      // Opus prediction cannot safely bridge a deliberately dropped packet.
      // Mark the first retained packet after the gap so the codec task resets
      // immediately before decoding that exact packet, without a timing race
      // against a frame that is already in flight.
      if (droppedForLatency) {
        if (!decodeQueue.empty()) {
          decodeQueue.front().resetDecoder = true;
        } else {
          packet.resetDecoder = true;
        }
      }
      queued = decodeQueue.push_back(std::move(packet));
      if (!queued) {
        ++droppedDecodePackets;
        decoderResetRequested.store(true);
      }
      xSemaphoreGive(queueMutex);
    }
    if (queued) {
      if (decoderTask != nullptr) {
        xTaskNotifyGive(decoderTask);
      }
    }
  }

  void cancelPlayback() {
    playbackSuppressed.store(true);
    playbackGeneration.fetch_add(1);
    decoderResetRequested.store(true);
    // Mute before waiting for queue/decoder synchronization so the audible
    // abort latency is independent of a codec task already in flight.
    applyCodecMute(true);
    if (queueMutex != nullptr &&
        xSemaphoreTake(queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      decodeQueue.clear();
      while (!playbackQueue.empty()) {
        playbackPool.push_back(std::move(playbackQueue.front().pcm));
        playbackQueue.pop_front();
      }
      clearPlaybackTimestampsLocked();
      xSemaphoreGive(queueMutex);
    }
    if (decoderTask != nullptr) {
      xTaskNotifyGive(decoderTask);
    }
    if (outputTask != nullptr) {
      xTaskNotifyGive(outputTask);
    }
  }

  void clearCaptureQueues() {
    if (queueMutex == nullptr ||
        xSemaphoreTake(queueMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      return;
    }
    while (!encodeQueue.empty()) {
      capturePool.push_back(std::move(encodeQueue.front().pcm));
      encodeQueue.pop_front();
    }
    while (!uplinkQueue.empty()) {
      uplinkPool.push_back(std::move(uplinkQueue.front().opus));
      uplinkQueue.pop_front();
    }
    xSemaphoreGive(queueMutex);
  }

  void drainUplink() {
    for (size_t count = 0; count < config.uplinkPacketsPerLoop; ++count) {
      UplinkPacket packet;
      if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        if (!uplinkQueue.empty()) {
          packet = std::move(uplinkQueue.front());
          uplinkQueue.pop_front();
        }
        xSemaphoreGive(queueMutex);
      }
      if (packet.opus.empty()) {
        break;
      }
      if (!captureEnabled.load() ||
          packet.generation != captureGeneration.load()) {
        ++discardedStaleCapturePackets;
      } else {
        // A playback abort may race capture/encode without changing the capture
        // generation in realtime mode. Preserve the audio but never attach a
        // timestamp from an invalidated playback generation.
        const uint32_t timestamp =
            packet.timestampGeneration == playbackGeneration.load()
                ? packet.timestamp
                : 0;
        if (!uplink(packet.opus.data(), packet.opus.size(), timestamp)) {
          ++rejectedUplinkPackets;
        }
      }
      if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
        uplinkPool.push_back(std::move(packet.opus));
        xSemaphoreGive(queueMutex);
      }
    }
  }

  uint32_t queuedPlaybackDurationMs() {
    if (queueMutex == nullptr) {
      return 0;
    }
    uint32_t duration = 0;
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) {
      for (const auto& packet : decodeQueue) {
        duration += packet.frame.format.frame_duration_ms;
      }
      for (const auto& chunk : playbackQueue) {
        duration += chunk.durationMs;
      }
      xSemaphoreGive(queueMutex);
    }
    return duration;
  }

  void logPerformance() {
    const uint32_t now = millis();
    if (now - lastPerformanceLogMs < config.performanceLogIntervalMs) {
      return;
    }
    lastPerformanceLogMs = now;
    // USB CDC writes may block when the host is not consuming them. Keep the
    // Client loop and its uplink queue moving when no serial monitor is open.
    if (!Serial) {
      return;
    }
    size_t encodeDepth = 0;
    size_t uplinkDepth = 0;
    size_t decodeDepth = 0;
    size_t playbackDepth = 0;
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      encodeDepth = encodeQueue.size();
      uplinkDepth = uplinkQueue.size();
      decodeDepth = decodeQueue.size();
      playbackDepth = playbackQueue.size();
      xSemaphoreGive(queueMutex);
    }
    Serial.printf("[audio-perf] q enc=%u up=%u dec=%u play=%u "
                  "drop enc=%lu up=%lu dec=%lu stale=%lu reject=%lu "
                  "err in=%lu enc=%lu dec=%lu out=%lu played=%lu "
                  "max_us enc=%lu dec=%lu wake=%lu heap=%lu min=%lu maxblk=%lu "
                  "cap n=%lu peak=%lu rms=%lu raw=%lu opus=%lu "
                  "stack input=%u codec=%u output=%u wake=%u loop=%u\n",
                  static_cast<unsigned>(encodeDepth),
                  static_cast<unsigned>(uplinkDepth),
                  static_cast<unsigned>(decodeDepth),
                  static_cast<unsigned>(playbackDepth),
                  static_cast<unsigned long>(droppedEncodePackets.load()),
                  static_cast<unsigned long>(droppedUplinkPackets.load()),
                  static_cast<unsigned long>(droppedDecodePackets.load()),
                  static_cast<unsigned long>(discardedStaleCapturePackets.load()),
                  static_cast<unsigned long>(rejectedUplinkPackets.load()),
                  static_cast<unsigned long>(inputErrors.load()),
                  static_cast<unsigned long>(encodeErrors.load()),
                  static_cast<unsigned long>(decodeErrors.load()),
                  static_cast<unsigned long>(outputErrors.load()),
                  static_cast<unsigned long>(playedPackets.load()),
                  static_cast<unsigned long>(maximumEncodeUs.load()),
                  static_cast<unsigned long>(maximumDecodeUs.load()),
                  static_cast<unsigned long>(maximumWakeDetectUs.load()),
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()),
                  static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                  static_cast<unsigned long>(capturedPackets.load()),
                  static_cast<unsigned long>(latestCapturePeak.load()),
                  static_cast<unsigned long>(latestCaptureRms.load()),
                  static_cast<unsigned long>(latestCaptureRawRms.load()),
                  static_cast<unsigned long>(latestCaptureOpusBytes.load()),
                  inputTask == nullptr ? 0 : uxTaskGetStackHighWaterMark(inputTask),
                  decoderTask == nullptr ? 0 : uxTaskGetStackHighWaterMark(decoderTask),
                  outputTask == nullptr ? 0 : uxTaskGetStackHighWaterMark(outputTask),
                  wakeTask == nullptr ? 0 : uxTaskGetStackHighWaterMark(wakeTask),
                  uxTaskGetStackHighWaterMark(nullptr));
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
    return drained && millis() - lastPlaybackWriteMs.load() > config.playbackIdleDelayMs;
  }
};

I2sOpusAudioPort::I2sOpusAudioPort(const Config& config) : impl_(new Impl(config)) {}

I2sOpusAudioPort::~I2sOpusAudioPort() {
  end();
  delete impl_;
}

bool I2sOpusAudioPort::begin(const xiaozhi::AudioFormat& captureFormat, Uplink uplink) {
  const Config& config = impl_->config;
  const uint32_t inputRate = impl_->inputSampleRate();
  const uint32_t outputRate = impl_->outputSampleRate();
  const uint64_t hardwareSampleProduct =
      static_cast<uint64_t>(inputRate) * captureFormat.frame_duration_ms;
  const uint64_t captureSampleProduct =
      static_cast<uint64_t>(captureFormat.sample_rate) * captureFormat.frame_duration_ms;
  const bool validFrameSize = inputRate > 0 && outputRate > 0 &&
                                captureFormat.sample_rate > 0 &&
                                hardwareSampleProduct % 1000 == 0 &&
                                captureSampleProduct % 1000 == 0 &&
                                (static_cast<uint64_t>(config.readFramesPerLoop) *
                                 captureFormat.sample_rate) %
                                       inputRate ==
                                    0;
  const bool validWakeConfig = !config.enableWakeDetection ||
                                (config.wakeModelPartition != nullptr &&
                                 config.defaultWakeWord != nullptr &&
                                 config.wakeTaskStackBytes > 0);
  const bool validPipelineConfig =
      config.mclkMultiple > 0 &&
      config.dmaDescriptorCount > 0 && config.dmaFrames > 0 &&
      config.readFramesPerLoop > 0 && config.maximumDecodeQueueMs > 0 &&
      config.maximumPlaybackChunks > 0 && config.maximumEncodePackets > 0 &&
      config.maximumUplinkPackets > 0 && config.uplinkPacketsPerLoop > 0 &&
      config.inputTaskStackBytes > 0 && config.decoderTaskStackBytes > 0 &&
      config.outputTaskStackBytes > 0 && config.performanceLogIntervalMs > 0 &&
      config.autoChannelSwitchRatio >= 1.0f &&
      config.captureLogIntervalPackets > 0 &&
      (!config.enableSpeechConditioning ||
       (config.speechGateRms > 0 && config.speechTargetRms > 0 &&
        config.speechMaximumGain >= 1.0f && config.speechSilenceGain >= 0.0f &&
         config.speechSilenceGain <= 1.0f && config.speechStartPackets > 0 &&
         config.speechHoldMs > 0));
  std::string hardwareError;
  const bool validHardwareConfig = impl_->validateHardwareConfig(hardwareError);
  const size_t inputChannels =
      config.hardware.inputMode == InputMode::I2sPdm
          ? 1
          : config.hardware.input.channels;
  const size_t outputChannels = config.hardware.output.channels;
  if (impl_->started || !uplink || captureFormat.sample_rate == 0 ||
      captureFormat.channels != 1 ||
      i2s_opus_detail::encoderDuration(captureFormat.frame_duration_ms) ==
          ESP_OPUS_ENC_FRAME_DURATION_ARG ||
      !validFrameSize || !validWakeConfig || !validPipelineConfig ||
      !validHardwareConfig ||
      (config.captureChannel == CaptureChannel::Right && inputChannels != 2)) {
    Serial.printf("[audio] invalid format or configuration: %lu Hz, %u channel(s), %u ms\n",
                   static_cast<unsigned long>(captureFormat.sample_rate), captureFormat.channels,
                   captureFormat.frame_duration_ms);
    if (!hardwareError.empty()) {
      Serial.printf("[audio] hardware: %s\n", hardwareError.c_str());
    } else {
      Serial.println("[audio] check frame sizing, queues, wake settings, and capture channel");
    }
    return false;
  }
  impl_->inputChannels = inputChannels;
  impl_->outputChannels = outputChannels;
  impl_->hardwareFramesPerPacket =
      static_cast<size_t>(hardwareSampleProduct / 1000);
  impl_->captureSamplesPerPacket = static_cast<size_t>(captureSampleProduct / 1000);
  impl_->captured.resize(impl_->hardwareFramesPerPacket * inputChannels);
  impl_->readBuffer.resize(config.readFramesPerLoop * inputChannels);
  if (config.hardware.inputMode == InputMode::I2sStandard &&
      config.hardware.input.dataBits == 32) {
    impl_->readBuffer32.resize(impl_->readBuffer.size());
  } else {
    impl_->readBuffer32.clear();
  }
  if (config.hardware.output.dataBits == 32) {
    impl_->writeBuffer32.reserve(
        static_cast<size_t>(outputRate) * 60 / 1000 * outputChannels);
  } else {
    impl_->writeBuffer32.clear();
  }
  if (config.enableWakeDetection) {
    impl_->wakeCaptured.resize(impl_->readBuffer.size());
    impl_->wakeMono.resize(config.readFramesPerLoop * captureFormat.sample_rate /
                           inputRate);
  } else {
    impl_->wakeCaptured.clear();
    impl_->wakeMono.clear();
  }
  impl_->captureFormat = captureFormat;
  impl_->initializeCaptureFilter();
  impl_->uplink = std::move(uplink);
  if (!impl_->beginI2s() || !impl_->beginCodec() || !impl_->beginEncoder()) {
    impl_->stop();
    return false;
  }
  impl_->initializeBufferPools();
  if (!impl_->startWorkers() || !impl_->beginWakeDetection()) {
    impl_->stop();
    return false;
  }
  impl_->started = true;
  const char channelName = config.captureChannel == CaptureChannel::Left
                               ? 'L'
                               : (config.captureChannel == CaptureChannel::Right ? 'R'
                                                                                 : 'A');
  Serial.printf("[audio] profile=%s input=%lu Hz output=%lu Hz capture=%lu Hz "
                "frame=%u ms channel=%c\n",
                Config::compiledProfileName(),
                static_cast<unsigned long>(inputRate),
                static_cast<unsigned long>(outputRate),
                static_cast<unsigned long>(captureFormat.sample_rate),
                captureFormat.frame_duration_ms, channelName);
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
  impl_->drainUplink();
  impl_->logPerformance();
}

void I2sOpusAudioPort::setCaptureEnabled(bool enabled) {
  if (!impl_->started) {
    return;
  }
  if (enabled) {
    if (impl_->captureEnabled.load()) {
      return;
    }
    // Capture-owned state is reset by the input task, avoiding a race with a
    // packet that was already being conditioned when the session changed.
    impl_->captureResetRequested.store(true);
    if (impl_->queueMutex != nullptr &&
        xSemaphoreTake(impl_->queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      impl_->clearPlaybackTimestampsLocked();
      xSemaphoreGive(impl_->queueMutex);
    }
    impl_->captureEnabled.store(true);
  } else {
    // Publish disabled before changing the generation. A capture that already
    // took the old generation can observe false; a later enable cannot make it
    // valid again because the generation changes before this call returns.
    if (!impl_->captureEnabled.exchange(false)) {
      return;
    }
    // Invalidate a packet that the codec task may already be encoding. It must
    // not be sent after the Client has entered Speaking or Idle.
    impl_->captureGeneration.fetch_add(1);
  }
  if (!enabled) {
    impl_->clearCaptureQueues();
  } else if (impl_->inputTask != nullptr) {
    xTaskNotifyGive(impl_->inputTask);
  }
  if (Serial) {
    Serial.printf("[audio] capture %s\n", enabled ? "enabled" : "disabled");
  }
}

void I2sOpusAudioPort::play(const xiaozhi::AudioFrame& frame) {
  if (impl_->started) {
    impl_->enqueueDecode(xiaozhi::AudioFrame(frame));
  }
}

void I2sOpusAudioPort::play(xiaozhi::AudioFrame&& frame) {
  if (impl_->started) {
    impl_->enqueueDecode(std::move(frame));
  }
}

void I2sOpusAudioPort::cancelPlayback() {
  if (impl_ != nullptr && impl_->started) {
    impl_->cancelPlayback();
  }
}

bool I2sOpusAudioPort::playbackIdle() const {
  return impl_ == nullptr || !impl_->started ||
         impl_->isPlaybackIdle();
}

uint32_t I2sOpusAudioPort::queuedPlaybackMs() const {
  return impl_ == nullptr || !impl_->started
             ? 0
             : impl_->queuedPlaybackDurationMs();
}

void I2sOpusAudioPort::setWakeDetectionEnabled(bool enabled) {
  if (impl_ != nullptr && impl_->started) {
    impl_->setWakeDetection(enabled);
  }
}

bool I2sOpusAudioPort::consumeWakeWord(std::string& wakeWord) {
  return impl_ != nullptr && impl_->started && impl_->consumeWake(wakeWord);
}
