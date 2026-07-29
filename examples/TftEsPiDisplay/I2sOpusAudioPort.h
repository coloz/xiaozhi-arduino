#pragma once

#include <Xiaozhi.h>

#include <cstddef>
#include <cstdint>

class TwoWire;

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

  struct CodecFormat {
    uint32_t sampleRate = 0;
    uint8_t bitsPerSample = 16;
    uint8_t channels = 2;
    uint16_t mclkMultiple = 256;
  };

  struct CodecCallbacks {
    void* context = nullptr;
    bool (*begin)(void* context, const CodecFormat& format) = nullptr;
    bool (*setMuted)(void* context, bool muted) = nullptr;
    void (*end)(void* context) = nullptr;
  };

  struct Config {
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
    uint16_t maximumDecodeQueueMs = 2400;
    size_t maximumPlaybackChunks = 2;
    uint32_t decoderTaskStackBytes = 32 * 1024;
    uint32_t outputTaskStackBytes = 4096;
    uint8_t decoderTaskPriority = 2;
    uint8_t outputTaskPriority = 4;
    uint32_t playbackWriteTimeoutMs = 250;
    uint32_t playbackMuteDelayMs = 120;
    uint32_t playbackIdleDelayMs = 100;

    // Wake-word detection is optional, which lets boards without a compatible
    // model partition use the same audio port.
    bool enableWakeDetection = true;
    const char* wakeModelPartition = "model";
    const char* defaultWakeWord = "你好小智";
    WakeDetectionMode wakeDetectionMode = WakeDetectionMode::Aggressive;
    uint32_t wakeTaskStackBytes = 8 * 1024;
    uint8_t wakeTaskPriority = 5;
    int wakeTaskCore = 0;
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
  bool playbackIdle() const override;

  // Wake-word events are consumed from the Arduino loop so Client APIs are
  // never called by the speech-recognition task.
  void setWakeDetectionEnabled(bool enabled);
  bool consumeWakeWord(std::string& wakeWord);

 private:
  struct Impl;
  Impl* impl_;
};
