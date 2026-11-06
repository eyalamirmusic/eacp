#pragma once

/** @file KeyGrab: takes chosen keys away from a Window before its views see
    them. macOS and Windows. */

#include "KeyForwarding.h"
#include "Window.h"

namespace eacp::Graphics
{

// While alive, key-downs in `window` that `shouldGrab` accepts go to `onGrabbed`
// instead of the views, and so do their key-ups. Not while a text field is
// being edited. Destroying it sends a key-up for every key still held.
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
