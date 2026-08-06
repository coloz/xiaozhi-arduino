#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "Types.h"

namespace xiaozhi {

// Hardware PCM interface. Board packages can implement this with ESP_I2S or a codec library.
class AudioIo {
public:
    virtual ~AudioIo() = default;
    virtual bool begin(uint32_t input_sample_rate, uint32_t output_sample_rate) = 0;
    virtual void end() = 0;
    virtual size_t read(int16_t* samples, size_t sample_count) = 0;
    virtual size_t write(const int16_t* samples, size_t sample_count) = 0;
    virtual void setInputEnabled(bool enabled) = 0;
    virtual void setOutputEnabled(bool enabled) = 0;
};

// Opus stays a protocol requirement, but its implementation is injectable.
class OpusCodec {
public:
    virtual ~OpusCodec() = default;
    virtual bool begin(const AudioFormat& input, const AudioFormat& output) = 0;
    virtual void end() = 0;
    virtual bool encode(const int16_t* pcm, size_t sample_count, uint32_t timestamp,
                        AudioFrame& frame) = 0;
    virtual bool decode(const AudioFrame& frame, std::vector<int16_t>& pcm) = 0;
    virtual void resetDecoder() = 0;
};

// A complete optional audio extension exchanges encoded frames with Client.
// Client and Transport are single-task APIs: worker tasks may fill private
// queues, but Uplink must be invoked by loop() on the same task as Client::loop()
// (the ClientRuntime service task when a runtime owns the Client).
// end() must stop workers and quiesce every pending Uplink before returning.
class EncodedAudioPort {
public:
    using Uplink = std::function<bool(const uint8_t* opus, size_t size, uint32_t timestamp)>;

    virtual ~EncodedAudioPort() = default;
    virtual bool begin(const AudioFormat& capture_format, Uplink uplink) = 0;
    virtual void end() = 0;
    virtual void loop() = 0;
    virtual void setCaptureEnabled(bool enabled) = 0;
    virtual void play(const AudioFrame& frame) = 0;
    // Move-aware ports can override this overload to take ownership of the
    // encoded payload without another allocation. Existing ports remain source
    // compatible through the const-reference fallback.
    virtual void play(AudioFrame&& frame) { play(frame); }
    // Stop locally queued/in-flight playback after a user abort, disconnect, or
    // new TTS generation. Implementations should be idempotent and non-blocking
    // apart from the short synchronization needed to invalidate old work.
    virtual void cancelPlayback() {}
    // Return false while decoded audio is still queued or playing. Auto/realtime listening
    // waits for this signal before reopening capture, which reduces tail echo.
    virtual bool playbackIdle() const { return true; }
    // Best-effort observability for UI/diagnostics; zero is valid for ports that
    // do not expose their queue depth.
    virtual uint32_t queuedPlaybackMs() const { return 0; }
};

}  // namespace xiaozhi
