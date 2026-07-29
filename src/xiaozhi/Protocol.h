#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Types.h"

namespace xiaozhi {

struct ServerHello {
    std::string session_id;
    AudioFormat audio;
};

class Protocol {
public:
    static bool validateConfig(const ClientConfig& config, std::string& error);

    static bool makeHello(const ClientConfig& config, std::string& output,
                          std::string& error);
    static bool parseServerHello(const uint8_t* data, size_t size, ServerHello& output,
                                 std::string& error);

    static bool makeStartListening(const std::string& session_id, ListeningMode mode,
                                   std::string& output);
    static bool makeStopListening(const std::string& session_id, std::string& output);
    static bool makeWakeWordDetected(const std::string& session_id,
                                     const std::string& wake_word, std::string& output);
    static bool makeAbort(const std::string& session_id, AbortReason reason,
                          std::string& output);
    static bool makeMcp(const std::string& session_id, const std::string& payload,
                        std::string& output, std::string& error);

    static bool encodeAudio(uint8_t version, const uint8_t* opus, size_t opus_size,
                            uint32_t timestamp, size_t max_payload,
                            std::vector<uint8_t>& output, std::string& error);
    static bool decodeAudio(uint8_t version, const uint8_t* data, size_t size,
                            const AudioFormat& format, size_t max_payload,
                            AudioFrame& output, std::string& error);
};

}  // namespace xiaozhi
