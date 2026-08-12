#pragma once

#include <cstddef>
#include <cstdint>

#include "../Audio.h"
#include "../boards/AudioProfile.h"

class TwoWire;

// Select exactly one hardware profile before including this header.  Like a
// U8g2 constructor, the selected profile determines which backend code and
// optional dependencies are compiled into this sketch translation unit.
#ifndef XIAOZHI_AUDIO_PROFILE
#define XIAOZHI_AUDIO_PROFILE XIAOZHI_AUDIO_PROFILE_ES8311
#define XIAOZHI_AUDIO_LEGACY_DEFAULT_PROFILE 1
#endif

#if XIAOZHI_AUDIO_PROFILE < XIAOZHI_AUDIO_PROFILE_ES8311 || \
    XIAOZHI_AUDIO_PROFILE > XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC
#error "Unsupported XIAOZHI_AUDIO_PROFILE"
#endif

// Low-level feature flags are derived from the profile but remain overrideable
// for advanced custom profiles.  Values must be numeric 0/1 so #if really
// removes the unselected driver and its includes from the firmware.
#ifndef XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_PDM_I2S
#define XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD 0
#else
#define XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD 1
#endif
#endif

#ifndef XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_PDM_I2S
#define XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM 1
#else
#define XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM 0
#endif
#endif

#ifndef XIAOZHI_AUDIO_ENABLE_OUTPUT_I2S_STD
#define XIAOZHI_AUDIO_ENABLE_OUTPUT_I2S_STD 1
#endif

#ifndef XIAOZHI_AUDIO_ENABLE_I2S_SHARED
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_ES8311 || \
    XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_DUPLEX || \
    XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_CUSTOM_CODEC
#define XIAOZHI_AUDIO_ENABLE_I2S_SHARED 1
#else
#define XIAOZHI_AUDIO_ENABLE_I2S_SHARED 0
#endif
#endif

#ifndef XIAOZHI_AUDIO_ENABLE_I2S_SEPARATE
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX || \
    XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_PDM_I2S
#define XIAOZHI_AUDIO_ENABLE_I2S_SEPARATE 1
#else
#define XIAOZHI_AUDIO_ENABLE_I2S_SEPARATE 0
#endif
#endif

#ifndef XIAOZHI_AUDIO_ENABLE_CODEC_ES8311
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_ES8311
#define XIAOZHI_AUDIO_ENABLE_CODEC_ES8311 1
#else
#define XIAOZHI_AUDIO_ENABLE_CODEC_ES8311 0
#endif
#endif

#ifndef XIAOZHI_AUDIO_ENABLE_AMP_GPIO
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_DUPLEX || \
    XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX || \
    XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_PDM_I2S
#define XIAOZHI_AUDIO_ENABLE_AMP_GPIO 1
#else
#define XIAOZHI_AUDIO_ENABLE_AMP_GPIO 0
#endif
#endif

#ifndef XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR
#ifdef XIAOZHI_AUDIO_LEGACY_DEFAULT_PROFILE
#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 1
#else
#define XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR 0
#endif
#endif

#if (XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD != 0 && \
     XIAOZHI_AUDIO_ENABLE_INPUT_I2S_STD != 1) || \
    (XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM != 0 && \
     XIAOZHI_AUDIO_ENABLE_INPUT_I2S_PDM != 1) || \
    (XIAOZHI_AUDIO_ENABLE_OUTPUT_I2S_STD != 0 && \
     XIAOZHI_AUDIO_ENABLE_OUTPUT_I2S_STD != 1) || \
    (XIAOZHI_AUDIO_ENABLE_I2S_SHARED != 0 && \
     XIAOZHI_AUDIO_ENABLE_I2S_SHARED != 1) || \
    (XIAOZHI_AUDIO_ENABLE_I2S_SEPARATE != 0 && \
     XIAOZHI_AUDIO_ENABLE_I2S_SEPARATE != 1) || \
    (XIAOZHI_AUDIO_ENABLE_CODEC_ES8311 != 0 && \
     XIAOZHI_AUDIO_ENABLE_CODEC_ES8311 != 1) || \
    (XIAOZHI_AUDIO_ENABLE_AMP_GPIO != 0 && \
     XIAOZHI_AUDIO_ENABLE_AMP_GPIO != 1) || \
    (XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR != 0 && \
     XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR != 1)
#error "XIAOZHI_AUDIO_ENABLE_* macros must be numeric 0 or 1"
#endif

// I2S + Opus audio path with a configurable board/codec boundary.
//
// The built-in codec path supports ES8311. Boards using another codec can
// provide all three CodecCallbacks and keep the I2S/Opus pipeline unchanged.
class I2sOpusAudioPort final : public xiaozhi::EncodedAudioPort {
 public:
  enum class CaptureChannel : uint8_t {
    Auto,
    Left,
    Right,
  };

  enum class WakeDetectionMode : uint8_t {
    Balanced,
    Aggressive,
  };

  enum class InputMode : uint8_t {
    I2sStandard,
    I2sPdm,
  };

  enum class BusMode : uint8_t {
    Shared,
    Separate,
  };

  enum class CodecMode : uint8_t {
    None,
    Es8311,
    Custom,
  };

  enum class I2sSlot : uint8_t {
    Left,
    Right,
    Both,
  };

  // One standard-I2S direction. `dataBits` controls the DMA sample type;
  // `validBits` and `rightShift` describe a digital microphone's useful bits.
  // The pipeline always converts capture to mono PCM16 before resampling.
  struct StandardI2sEndpoint {
    int port = -1;
    uint32_t sampleRate = 0;
    int mclk = -1;
    int bclk = -1;
    int ws = -1;
    int data = -1;
    bool invertMclk = false;
    bool invertBclk = false;
    bool invertWs = false;
    uint8_t dataBits = 16;
    uint8_t validBits = 16;
    uint8_t slotBits = 16;
    uint8_t channels = 1;
    I2sSlot slot = I2sSlot::Left;
    uint8_t rightShift = 0;
  };

  struct PdmInputEndpoint {
    int port = -1;
    uint32_t sampleRate = 0;
    int clock = -1;
    int data = -1;
    bool invertClock = false;
  };

  struct AmplifierControl {
    int enablePin = -1;
    bool activeLevel = true;
    uint8_t volumePercent = 70;
  };

  struct Es8311Control {
    TwoWire* wire = nullptr;
    bool initializeWire = true;
    int i2cSda = -1;
    int i2cScl = -1;
    uint32_t i2cFrequency = 400000;
    uint8_t address = 0x18;
    bool resetOnBegin = true;
    uint16_t resetDelayMs = 10;
    int paPin = -1;
    bool paActiveLevel = true;
    bool useMclk = true;
    bool master = false;
    bool noDacReference = true;
    float paSupplyVoltage = 5.0f;
    float codecDacVoltage = 3.3f;
    float paGainDb = 0.0f;
    float microphoneGainDb = 30.0f;
    float outputVolumeDb = -12.0f;
  };

  struct HardwareConfig {
    InputMode inputMode = InputMode::I2sStandard;
    BusMode busMode = BusMode::Shared;
    CodecMode codecMode = CodecMode::Es8311;
    StandardI2sEndpoint input;
    StandardI2sEndpoint output;
    PdmInputEndpoint pdmInput;
    AmplifierControl amplifier;
    Es8311Control es8311;
  };

  struct CodecFormat {
    uint32_t sampleRate = 0;
    uint8_t bitsPerSample = 16;
    uint8_t channels = 2;
    uint16_t mclkMultiple = 256;
    uint32_t inputSampleRate = 0;
    uint32_t outputSampleRate = 0;
    uint8_t inputChannels = 0;
    uint8_t outputChannels = 0;
  };

  struct CodecCallbacks {
    void* context = nullptr;
    bool (*begin)(void* context, const CodecFormat& format) = nullptr;
    bool (*setMuted)(void* context, bool muted) = nullptr;
    void (*end)(void* context) = nullptr;
  };

  struct Config {
    // New code should start from forCompiledProfile(), then fill only the pins
    // and board-specific gains.  `hardware` is kept independent of the compile
    // macro so public Config layout remains stable across profiles.
    HardwareConfig hardware;
    bool useLegacyHardwareConfig = true;

    static Config forCompiledProfile();
    static const char* compiledProfileName();

    // Legacy ES8311/shared-I2S fields retained for source compatibility. They
    // are mapped into `hardware` when useLegacyHardwareConfig remains true.
    // I2C bus used by the built-in ES8311 codec path. Set initializeWire to
    // false when the application has already configured this bus.
    TwoWire* wire = nullptr;
    bool initializeWire = true;
    int i2cSda = -1;
    int i2cScl = -1;
    uint32_t i2cFrequency = 400000;
    uint8_t codecAddress = 0x18;

    // I2S standard-mode wiring and clocking. Both input and output are required.
    int i2sPort = 0;
    int mclk = -1;
    int bclk = -1;
    int ws = -1;
    int dataOut = -1;
    int dataIn = -1;
    bool invertMclk = false;
    bool invertBclk = false;
    bool invertWs = false;
    bool stereo = true;
    // Must be divisible by 100 so every supported 10/20/40/60 ms server frame
    // maps to an integer number of hardware samples without long-term drift.
    uint32_t hardwareSampleRate = 24000;
    uint16_t mclkMultiple = 256;
    uint8_t dmaDescriptorCount = 6;
    uint16_t dmaFrames = 240;
    size_t readFramesPerLoop = 240;

    // Capture and Opus encoder tuning. Capture is resampled from the hardware
    // rate to the format requested by Xiaozhi.
    CaptureChannel captureChannel = CaptureChannel::Auto;
    float autoChannelSwitchRatio = 2.0f;
    uint32_t opusBitrate = 24000;
    uint8_t opusComplexity = 0;
    bool opusFec = false;
    bool opusDtx = true;
    bool opusVbr = true;
    // Apply a small fixed-point low-pass before downsampling. This prevents
    // 8-12 kHz microphone energy from aliasing into the 16 kHz speech stream.
    bool captureAntiAlias = true;
    // Apply the same streaming low-pass when server audio must be downsampled
    // to the hardware rate (notably 48 kHz Opus to a 24 kHz codec).
    bool playbackAntiAlias = true;
    // Emit capture level diagnostics at this packet interval. At the default
    // 60 ms frame size, 50 packets gives an update every 3 seconds.
    uint16_t captureLogIntervalPackets = 50;

    // Optional packet-level speech conditioning for microphones whose speech
    // level is only slightly above their noise floor. Quiet packets are
    // attenuated so server-side VAD can stop promptly; detected speech is
    // raised toward speechTargetRms without exceeding speechMaximumGain.
    bool enableSpeechConditioning = false;
    uint32_t speechGateRms = 300;
    uint32_t speechTargetRms = 2200;
    float speechMaximumGain = 4.0f;
    float speechSilenceGain = 0.25f;
    uint8_t speechStartPackets = 2;
    uint16_t speechHoldMs = 600;

    // Built-in ES8311 settings. These are ignored when codec callbacks are set.
    bool resetCodecOnBegin = true;
    uint16_t codecResetDelayMs = 10;
    int codecPaPin = -1;
    bool codecUseMclk = true;
    bool codecMaster = false;
    bool codecNoDacReference = true;
    float paSupplyVoltage = 5.0f;
    float codecDacVoltage = 3.3f;
    float paGainDb = 0.0f;
    float microphoneGainDb = 30.0f;
    float outputVolumeDb = -12.0f;

    // Supply all three callbacks to use a different codec. The application
    // owns the callback context and must keep it alive while this port exists.
    CodecCallbacks codec;

    // Buffering and worker tasks.
    // Match xiaozhi-esp32 2.4.0's 2400 ms compressed-packet queues. Playback
    // still starts immediately and cancelPlayback() invalidates the complete
    // backlog, so this absorbs Wi-Fi/server bursts without slowing barge-in.
    uint16_t maximumDecodeQueueMs = 2400;
    size_t maximumPlaybackChunks = 2;
    size_t maximumEncodePackets = 2;
    // Forty packets is 2400 ms with the official/default 60 ms Opus frame.
    size_t maximumUplinkPackets = 40;
    // Keep work bounded, but drain faster than capture after a short stall.
    size_t uplinkPacketsPerLoop = 4;
    uint32_t inputTaskStackBytes = 8 * 1024;
    uint32_t decoderTaskStackBytes = 32 * 1024;
    uint32_t outputTaskStackBytes = 4096;
    uint8_t inputTaskPriority = 8;
    uint8_t decoderTaskPriority = 2;
    uint8_t outputTaskPriority = 4;
    uint32_t playbackWriteTimeoutMs = 250;
    uint32_t playbackMuteDelayMs = 600;
    uint32_t playbackIdleDelayMs = 100;
    uint32_t performanceLogIntervalMs = 10000;

    // Wake-word detection is optional, which lets boards without a compatible
    // model partition use the same audio port.
    bool enableWakeDetection = XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR != 0;
    const char* wakeModelPartition = "model";
    // Optional substring used to select one WakeNet model from the partition.
    // Leave null to keep the ESP-SR default (the first WakeNet model).
    const char* wakeModelKeyword = nullptr;
    const char* defaultWakeWord = "你好小智";
    WakeDetectionMode wakeDetectionMode = WakeDetectionMode::Aggressive;
    uint32_t wakeTaskStackBytes = 8 * 1024;
    uint8_t wakeTaskPriority = 5;
    int wakeTaskCore = 0;

    // Fixed downlink ownership budget. 1275 bytes is the maximum legal Opus
    // packet size. The queue has maximumDecodePackets slots, the pool has one
    // extra buffer for the decoder, and maximumDecodeQueueMs remains the
    // independent latency bound.
    size_t maximumDecodePackets = 42;
    size_t maximumDownlinkOpusBytes = 1275;
  };

  explicit I2sOpusAudioPort(const Config& config);
  ~I2sOpusAudioPort() override;

  I2sOpusAudioPort(const I2sOpusAudioPort&) = delete;
  I2sOpusAudioPort& operator=(const I2sOpusAudioPort&) = delete;

  bool begin(const xiaozhi::AudioFormat& captureFormat, Uplink uplink) override;
  void end() override;
  void loop() override;
  void setCaptureEnabled(bool enabled) override;
  void play(const xiaozhi::AudioFrame& frame) override;
  void play(xiaozhi::AudioFrame&& frame) override;
  void play(const xiaozhi::AudioFrameView& frame) override;
  void cancelPlayback() override;
  bool playbackIdle() const override;
  bool setPlaybackMuted(bool muted) override;
  uint32_t queuedPlaybackMs() const override;
  void onClientStateChanged(xiaozhi::State oldState,
                            xiaozhi::State newState) override;
  void setRealtimeControlSink(xiaozhi::RealtimeControlSink* sink) override;

  // Legacy polling fallback for direct Client users without ClientRuntime.
  void setWakeDetectionEnabled(bool enabled);
  bool consumeWakeWord(std::string& wakeWord);

 private:
  struct Impl;
  Impl* impl_;
};

inline I2sOpusAudioPort::Config
I2sOpusAudioPort::Config::forCompiledProfile() {
  Config config;
  config.useLegacyHardwareConfig = false;
  config.enableWakeDetection = XIAOZHI_AUDIO_ENABLE_WAKE_ESP_SR != 0;

  HardwareConfig& hardware = config.hardware;
  hardware.output.port = 0;
  hardware.output.sampleRate = 24000;
  hardware.output.slot = I2sSlot::Left;

#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_ES8311
  hardware.inputMode = InputMode::I2sStandard;
  hardware.busMode = BusMode::Shared;
  hardware.codecMode = CodecMode::Es8311;
  hardware.input.port = 0;
  hardware.input.sampleRate = 24000;
  hardware.input.dataBits = 16;
  hardware.input.validBits = 16;
  hardware.input.slotBits = 16;
  hardware.input.channels = 2;
  hardware.input.slot = I2sSlot::Both;
  hardware.output.dataBits = 16;
  hardware.output.validBits = 16;
  hardware.output.slotBits = 16;
  hardware.output.channels = 2;
  hardware.output.slot = I2sSlot::Both;
#elif XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_DUPLEX
  hardware.inputMode = InputMode::I2sStandard;
  hardware.busMode = BusMode::Shared;
  hardware.codecMode = CodecMode::None;
  hardware.input.port = 0;
  hardware.input.sampleRate = 24000;
  hardware.input.dataBits = 32;
  hardware.input.validBits = 24;
  hardware.input.slotBits = 32;
  hardware.input.channels = 1;
  hardware.input.slot = I2sSlot::Left;
  hardware.input.rightShift = 12;
  hardware.output.dataBits = 32;
  hardware.output.validBits = 16;
  hardware.output.slotBits = 32;
  hardware.output.channels = 1;
#elif XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
  hardware.inputMode = InputMode::I2sStandard;
  hardware.busMode = BusMode::Separate;
  hardware.codecMode = CodecMode::None;
  hardware.input.port = 1;
  hardware.input.sampleRate = 16000;
  hardware.input.dataBits = 32;
  hardware.input.validBits = 24;
  hardware.input.slotBits = 32;
  hardware.input.channels = 1;
  hardware.input.slot = I2sSlot::Left;
  hardware.input.rightShift = 12;
  hardware.output.dataBits = 32;
  hardware.output.validBits = 16;
  hardware.output.slotBits = 32;
  hardware.output.channels = 1;
#elif XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_PDM_I2S
  hardware.inputMode = InputMode::I2sPdm;
  hardware.busMode = BusMode::Separate;
  hardware.codecMode = CodecMode::None;
  hardware.pdmInput.port = 0;
  hardware.pdmInput.sampleRate = 16000;
  hardware.output.port = 1;
  hardware.output.dataBits = 32;
  hardware.output.validBits = 16;
  hardware.output.slotBits = 32;
  hardware.output.channels = 1;
#else
  hardware.inputMode = InputMode::I2sStandard;
  hardware.busMode = BusMode::Shared;
  hardware.codecMode = CodecMode::Custom;
  hardware.input.port = 0;
  hardware.input.sampleRate = 24000;
  hardware.input.dataBits = 16;
  hardware.input.validBits = 16;
  hardware.input.slotBits = 16;
  hardware.input.channels = 2;
  hardware.input.slot = I2sSlot::Both;
  hardware.output.dataBits = 16;
  hardware.output.validBits = 16;
  hardware.output.slotBits = 16;
  hardware.output.channels = 2;
  hardware.output.slot = I2sSlot::Both;
#endif
  return config;
}

inline const char* I2sOpusAudioPort::Config::compiledProfileName() {
#if XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_ES8311
  return "ES8311 duplex";
#elif XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_DUPLEX
  return "standard I2S shared-clock mic + speaker";
#elif XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_I2S_SIMPLEX
  return "standard I2S mic + speaker (separate controllers)";
#elif XIAOZHI_AUDIO_PROFILE == XIAOZHI_AUDIO_PROFILE_PDM_I2S
  return "PDM mic + standard I2S speaker";
#else
  return "custom codec on standard I2S";
#endif
}
