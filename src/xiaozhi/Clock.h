#pragma once

#include <chrono>
#include <cstdint>

namespace xiaozhi {

class Clock {
public:
    virtual ~Clock() = default;
    virtual uint64_t nowMs() const = 0;
};

class SteadyClock final : public Clock {
public:
    uint64_t nowMs() const override {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
};

}  // namespace xiaozhi
