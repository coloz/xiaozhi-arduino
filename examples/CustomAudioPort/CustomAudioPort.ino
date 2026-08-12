/*
 * CustomAudioPort 示例
 *
 * 演示如何实现 xiaozhi::EncodedAudioPort，并把自定义音频输入、输出接入
 * xiaozhi::Client。此草图仅是接口骨架：MyTransport 不会建立连接，实际使用前
 * 还需补充传输逻辑，以及 PCM/Opus 的采集、编解码和播放实现。
 *
 * Demonstrates how to implement xiaozhi::EncodedAudioPort and connect custom audio
 * input and output to xiaozhi::Client. This sketch is only an interface skeleton:
 * MyTransport does not connect, so a real transport plus PCM/Opus capture, codec,
 * and playback logic must be added before use.
 */

#include <Xiaozhi.h>

class MyAudioPort final : public xiaozhi::EncodedAudioPort {
 public:
  bool begin(const xiaozhi::AudioFormat& captureFormat, Uplink uplink) override {
    capture_format_ = captureFormat;
    uplink_ = std::move(uplink);
    return true;
  }
  void end() override { uplink_ = {}; }
  void loop() override {
    // Read PCM, encode exactly capture_format_.frame_duration_ms to Opus,
    // then call uplink_(opus, size, timestamp). Keep this method non-blocking.
  }
  void setCaptureEnabled(bool enabled) override { capture_enabled_ = enabled; }
  void play(const xiaozhi::AudioFrame& frame) override {
    const xiaozhi::AudioFrameView view{
        frame.format, frame.timestamp, frame.opus.data(), frame.opus.size()};
    play(view);
  }
  void play(const xiaozhi::AudioFrameView& frame) override {
    // frame.opus is borrowed only until this call returns. Decode it now, or
    // synchronously copy it into a bounded worker buffer before returning.
    // Never retain the pointer in an asynchronous queue.
    last_downlink_size_ = frame.opus_size;
  }

 private:
  xiaozhi::AudioFormat capture_format_;
  Uplink uplink_;
  bool capture_enabled_ = false;
  size_t last_downlink_size_ = 0;
};

class MyTransport final : public xiaozhi::Transport {
 public:
  void setCallbacks(xiaozhi::TransportCallbacks callbacks) override {
    callbacks_ = std::move(callbacks);
  }
  bool connect(const xiaozhi::TransportRequest&) override { return false; }
  void loop() override {}
  bool sendText(const uint8_t*, size_t) override { return false; }
  bool sendBinary(const uint8_t*, size_t) override { return false; }
  void close() override {}
  bool connected() const override { return false; }

 private:
  xiaozhi::TransportCallbacks callbacks_;
};

MyTransport transport;
MyAudioPort audio;
xiaozhi::Client client(transport);

void setup() {
  client.attachAudioPort(&audio);
}

void loop() {}
