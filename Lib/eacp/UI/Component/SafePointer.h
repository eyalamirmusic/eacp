#pragma once

#include "Component.h"

#include <type_traits>

namespace eacp::UI
{
// A pointer to a component that turns null once the component is destroyed,
// as juce::Component::SafePointer. What a callback that outlives the code that
// made it holds: a click handler that may delete its own button, a callAsync
// that lands after the panel it was for has closed.
//
// It goes null when Component's destructor starts, which is after the derived
// class's destructor has run -- so while a derived destructor is running, the
// pointer still answers. Message-thread only, like the component itself.
template <typename T>
class SafePointer
{
    static_assert(std::is_base_of_v<Component, T>,
                  "SafePointer points at components");

public:
    SafePointer() = default;

    SafePointer(T* componentToUse)
        : component(componentToUse)
        , token(tokenFor(componentToUse))
    {
    }

    SafePointer(T& componentToUse)
        : SafePointer(&componentToUse)
    {
    }

    SafePointer& operator=(T* componentToUse)
    {
        component = componentToUse;
        token = tokenFor(componentToUse);
        return *this;
    }

    T* get() const { return token.expired() ? nullptr : component; }

    operator T*() const { return get(); }
    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }

private:
    static std::weak_ptr<bool> tokenFor(T* componentToUse)
    {
        if (componentToUse == nullptr)
            return {};

        return static_cast<const Component*>(componentToUse)->getLifetimeToken();
    }

    T* component = nullptr;
    std::weak_ptr<bool> token;
};
} // namespace eacp::UI
