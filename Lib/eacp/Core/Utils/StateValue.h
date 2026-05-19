#pragma once

#include "Broadcaster.h"

#include <utility>

namespace eacp
{

template <typename T>
class StateValue
{
public:
    explicit StateValue(T initial = {}) : value(std::move(initial)) {}

    StateValue(const StateValue&) = delete;
    StateValue& operator=(const StateValue&) = delete;
    StateValue(StateValue&&) = delete;
    StateValue& operator=(StateValue&&) = delete;

    const T& get() const noexcept { return value; }

    void set(T next)
    {
        value = std::move(next);
        changeBroadcaster.trigger();
    }

    template <typename Mutator>
    void modify(Mutator&& mutator)
    {
        mutator(value);
        changeBroadcaster.trigger();
    }

    Broadcaster& onChange() { return changeBroadcaster; }

private:
    T value;
    Broadcaster changeBroadcaster;
};

} // namespace eacp
