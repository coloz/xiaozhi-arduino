#pragma once

#include <string>

namespace xiaozhi {

class Esp32Identity {
public:
    static std::string deviceId();
    static std::string persistentClientId();
    static std::string firmwareVersion();

private:
    static std::string generateUuid();
};

}  // namespace xiaozhi
