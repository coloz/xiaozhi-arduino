#pragma once

#include <cstdint>
#include <string>

namespace xiaozhi {

// Stable local identifiers for Xiaozhi's standard emotion names. The numeric
// values are intended for application-side state/serial protocols; the
// Xiaozhi wire protocol continues to use the lowercase string names.
enum class Emotion : uint8_t {
    Unknown = 0,
    Neutral = 1,
    Happy = 2,
    Laughing = 3,
    Funny = 4,
    Sad = 5,
    Angry = 6,
    Crying = 7,
    Loving = 8,
    Embarrassed = 9,
    Surprised = 10,
    Shocked = 11,
    Thinking = 12,
    Winking = 13,
    Cool = 14,
    Relaxed = 15,
    Delicious = 16,
    Kissy = 17,
    Confident = 18,
    Sleepy = 19,
    Silly = 20,
    Confused = 21,
};

// Converts a protocol emotion name to its typed value. Matching is exact and
// case-sensitive because standard wire names are lowercase. Unknown, empty,
// and null names return Emotion::Unknown.
Emotion emotionFromName(const char* name);

inline Emotion emotionFromName(const std::string& name) {
    return emotionFromName(name.c_str());
}

// Returns the standard wire name. Emotion::Unknown returns "unknown".
const char* emotionName(Emotion emotion);

inline bool isKnownEmotion(Emotion emotion) {
    return emotion != Emotion::Unknown;
}

}  // namespace xiaozhi
