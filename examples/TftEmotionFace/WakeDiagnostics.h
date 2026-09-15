#pragma once

// Opt-in, local-only PCM capture for acoustic debugging. Nothing is recorded
// until 'r' is received over Serial; 's' stops and 'd' exports the capture.
#include <atomic>
#include <algorithm>
#include <mbedtls/base64.h>

namespace wake_diagnostics {
constexpr size_t kSamples = 16000 * 40;
int16_t* input = nullptr;
int16_t* output = nullptr;
std::atomic<size_t> inputSamples{0};
std::atomic<size_t> outputSamples{0};
std::atomic<size_t> inputWriteOffset{0};
std::atomic<size_t> outputWriteOffset{0};
std::atomic<unsigned> writers{0};
std::atomic<bool> recording{false};
uint32_t startedMs = 0;
bool reportReady = false;
bool dumping = false;
unsigned dumpStage = 0;
size_t dumpOffset = 0;

void observe(const int16_t* pcm, size_t samples, bool processed) {
  writers.fetch_add(1);
  if (recording.load()) {
    auto& count = processed ? outputSamples : inputSamples;
    auto& cursor = processed ? outputWriteOffset : inputWriteOffset;
    const size_t offset = cursor.load();
    const size_t capacity = processed ? kSamples : kSamples * 2;
    auto* buffer = processed ? output : input;
    // Keep the latest 40 seconds until 's'. A fixed wall-clock window often
    // expired before the person at the device had started the requested test.
    const size_t length = std::min(samples, capacity);
    pcm += samples - length;
    const size_t first = std::min(length, capacity - offset);
    std::copy_n(pcm, first, buffer + offset);
    std::copy_n(pcm + first, length - first, buffer);
    cursor.store((offset + length) % capacity);
    count.store(std::min(capacity, count.load() + length));
  }
  writers.fetch_sub(1);
}

bool command(char command) {
  if (command == 'r') {
    if (recording.load() || dumping || writers.load() != 0) {
      Serial.println("[wake-pcm] busy");
      return true;
    }
    if (input == nullptr) {
      input = static_cast<int16_t*>(heap_caps_malloc(kSamples * 4, MALLOC_CAP_SPIRAM));
    }
    if (output == nullptr) {
      output = static_cast<int16_t*>(heap_caps_malloc(kSamples * 2, MALLOC_CAP_SPIRAM));
    }
    if (input == nullptr || output == nullptr) {
      Serial.println("[wake-pcm] allocation failed");
      return true;
    }
    inputSamples.store(0);
    outputSamples.store(0);
    inputWriteOffset.store(0);
    outputWriteOffset.store(0);
    startedMs = millis();
    reportReady = true;
    recording.store(true);
    Serial.println("[wake-pcm] recording; retaining latest 40 seconds until stop");
    return true;
  }
  if (command == 's') {
    recording.store(false);
    return true;
  }
  if (command == 'd') {
    if (!recording.load() && writers.load() == 0 && inputSamples.load() != 0) {
      dumping = true;
      dumpStage = 0;
      dumpOffset = 0;
      Serial.printf("[wake-pcm] begin %u %u\n",
                    static_cast<unsigned>(inputSamples.load() * 2),
                    static_cast<unsigned>(outputSamples.load() * 2));
    }
    return true;
  }
  return false;
}

bool exportChunk() {
  const size_t total = (dumpStage == 0 ? inputSamples.load() : outputSamples.load()) * 2;
  if (dumpOffset >= total) {
    if (++dumpStage == 2) {
      dumping = false;
      Serial.println("[wake-pcm] end");
    }
    dumpOffset = 0;
    return dumping;
  }
  const size_t bytes = std::min<size_t>(96, total - dumpOffset);
  const auto* data = reinterpret_cast<const uint8_t*>(dumpStage == 0 ? input : output);
  const size_t capacity = kSamples * (dumpStage == 0 ? 4 : 2);
  const size_t cursor = (dumpStage == 0 ? inputWriteOffset.load() : outputWriteOffset.load()) * 2;
  const size_t offset = ((total == capacity ? cursor : 0) + dumpOffset) % capacity;
  uint8_t contiguous[96];
  const size_t first = std::min(bytes, capacity - offset);
  std::copy_n(data + offset, first, contiguous);
  std::copy_n(data, bytes - first, contiguous + first);
  unsigned char encoded[129];
  size_t encodedSize = 0;
  mbedtls_base64_encode(encoded, sizeof(encoded), &encodedSize, contiguous, bytes);
  char line[180];
  const int length = snprintf(line, sizeof(line), "[pcm:%u:%u] %s\n", dumpStage,
                              static_cast<unsigned>(dumpOffset), encoded);
  if (Serial.availableForWrite() >= length) {
    if (Serial.write(reinterpret_cast<const uint8_t*>(line), length) == static_cast<size_t>(length)) {
      dumpOffset += bytes;
      return true;
    }
  }
  return false;
}

void loop() {
  if (reportReady && !recording.load() && writers.load() == 0) {
    reportReady = false;
    Serial.printf("[wake-pcm] ready %u %u\n",
                  static_cast<unsigned>(inputSamples.load() * 2),
                  static_cast<unsigned>(outputSamples.load() * 2));
  }
  // Drain a bounded burst without waiting for USB; leave the audio tasks and
  // Runtime responsive. A 256-byte USB ring previously limited this to one
  // short line per iteration and full recordings exceeded the host timeout.
  for (unsigned chunk = 0; dumping && chunk < 8; ++chunk) {
    if (!exportChunk()) { break; }
  }
}
}  // namespace wake_diagnostics
