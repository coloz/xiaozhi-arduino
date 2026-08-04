#include "Emotion.h"

#include <cstring>

namespace xiaozhi {

Emotion emotionFromName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return Emotion::Unknown;
    }

    struct Entry {
        const char* name;
        Emotion emotion;
    };
    static constexpr Entry kEmotions[] = {
        {"neutral", Emotion::Neutral},
        {"happy", Emotion::Happy},
        {"laughing", Emotion::Laughing},
        {"funny", Emotion::Funny},
        {"sad", Emotion::Sad},
        {"angry", Emotion::Angry},
        {"crying", Emotion::Crying},
        {"loving", Emotion::Loving},
        {"embarrassed", Emotion::Embarrassed},
        {"surprised", Emotion::Surprised},
        {"shocked", Emotion::Shocked},
        {"thinking", Emotion::Thinking},
        {"winking", Emotion::Winking},
        {"cool", Emotion::Cool},
        {"relaxed", Emotion::Relaxed},
        {"delicious", Emotion::Delicious},
        {"kissy", Emotion::Kissy},
        {"confident", Emotion::Confident},
        {"sleepy", Emotion::Sleepy},
        {"silly", Emotion::Silly},
        {"confused", Emotion::Confused},
    };

    for (const Entry& entry : kEmotions) {
        if (std::strcmp(name, entry.name) == 0) {
            return entry.emotion;
        }
    }
    return Emotion::Unknown;
}

const char* emotionName(Emotion emotion) {
    switch (emotion) {
        case Emotion::Neutral:
            return "neutral";
        case Emotion::Happy:
            return "happy";
        case Emotion::Laughing:
            return "laughing";
        case Emotion::Funny:
            return "funny";
        case Emotion::Loving:
            return "loving";
        case Emotion::Embarrassed:
            return "embarrassed";
        case Emotion::Confident:
            return "confident";
        case Emotion::Delicious:
            return "delicious";
        case Emotion::Sad:
            return "sad";
        case Emotion::Crying:
            return "crying";
        case Emotion::Sleepy:
            return "sleepy";
        case Emotion::Silly:
            return "silly";
        case Emotion::Angry:
            return "angry";
        case Emotion::Surprised:
            return "surprised";
        case Emotion::Shocked:
            return "shocked";
        case Emotion::Thinking:
            return "thinking";
        case Emotion::Winking:
            return "winking";
        case Emotion::Cool:
            return "cool";
        case Emotion::Relaxed:
            return "relaxed";
        case Emotion::Kissy:
            return "kissy";
        case Emotion::Confused:
            return "confused";
        case Emotion::Unknown:
            return "unknown";
    }
    return "unknown";
}

}  // namespace xiaozhi
