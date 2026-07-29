#include "StateMachine.h"

#include <algorithm>

namespace xiaozhi {

State StateMachine::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool StateMachine::isValidTransition(State from, State to) {
    if (from == to) {
        return true;
    }
    // FatalError is an emergency sink and must be reachable from every live state.
    if (to == State::FatalError) {
        return from != State::FatalError;
    }

    switch (from) {
        case State::Unknown:
            return to == State::Starting;
        case State::Starting:
            return to == State::WifiConfiguring || to == State::Activating;
        case State::WifiConfiguring:
            return to == State::Activating || to == State::AudioTesting;
        case State::AudioTesting:
            return to == State::WifiConfiguring;
        case State::Activating:
            return to == State::Upgrading || to == State::Idle ||
                   to == State::WifiConfiguring;
        case State::Upgrading:
            return to == State::Idle || to == State::Activating;
        case State::Idle:
            return to == State::Connecting || to == State::Listening ||
                   to == State::Speaking || to == State::Activating ||
                   to == State::Upgrading || to == State::WifiConfiguring;
        case State::Connecting:
            return to == State::Idle || to == State::Listening;
        case State::Listening:
            return to == State::Speaking || to == State::Idle;
        case State::Speaking:
            return to == State::Listening || to == State::Idle;
        case State::FatalError:
            return false;
    }
    return false;
}

bool StateMachine::canTransitionTo(State target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isValidTransition(state_, target);
}

bool StateMachine::transitionTo(State target) {
    State previous;
    std::vector<Listener> listeners;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = state_;
        if (!isValidTransition(previous, target)) {
            return false;
        }
        if (previous == target) {
            return true;
        }
        state_ = target;
        listeners.reserve(listeners_.size());
        for (const auto& item : listeners_) {
            listeners.push_back(item.second);
        }
    }

    for (const auto& listener : listeners) {
        if (listener) {
            listener(previous, target);
        }
    }
    return true;
}

int StateMachine::addListener(Listener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int id = next_listener_id_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void StateMachine::removeListener(int listener_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
                       [listener_id](const auto& item) { return item.first == listener_id; }),
        listeners_.end());
}

}  // namespace xiaozhi
