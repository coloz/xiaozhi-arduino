#pragma once

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "Types.h"

namespace xiaozhi {

class StateMachine {
public:
    using Listener = std::function<void(State old_state, State new_state)>;

    StateMachine() = default;
    StateMachine(const StateMachine&) = delete;
    StateMachine& operator=(const StateMachine&) = delete;

    State state() const;
    bool canTransitionTo(State target) const;
    bool transitionTo(State target);
    int addListener(Listener listener);
    void removeListener(int listener_id);

    static bool isValidTransition(State from, State to);

private:
    mutable std::mutex mutex_;
    State state_ = State::Unknown;
    std::vector<std::pair<int, Listener>> listeners_;
    int next_listener_id_ = 1;
};

}  // namespace xiaozhi
