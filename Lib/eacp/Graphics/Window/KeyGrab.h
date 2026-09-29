#pragma once

#include "KeyForwarding.h"
#include "Window.h"

namespace eacp::Graphics
{

class KeyGrab
{
public:
    using Predicate = std::function<bool(const KeyEvent&)>;
    using Handler = std::function<void(const NativeKeyEvent&)>;

    KeyGrab(Window& window, Predicate shouldGrab, Handler onGrabbed);
    ~KeyGrab();

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
