#pragma once

/** @file A system-wide keyboard shortcut; real on macOS and Windows, a no-op on
    Linux, iOS and Android. */

#include "../Graphics/Keyboard.h"

namespace eacp::Graphics
{

// A process-owned global/system hotkey. Construct one and keep it alive for as
// long as the key should be registered; destroying it unregisters it.
// Callbacks fire on the main thread. `keyCode` is a Graphics::KeyCode
// constant (e.g. KeyCode::A); the values are macOS virtual key codes, and
// Windows maps them to its own. If the system
// refuses the registration it logs and the callback never fires; there is no
// error to query. Linux, iOS and Android accept
// it and never call back.
class GlobalHotKey
{
public:
    GlobalHotKey(ModifierKeys modifiers, uint16_t keyCode, Callback onPressed);
    ~GlobalHotKey();

    GlobalHotKey(const GlobalHotKey&) = delete;
    GlobalHotKey& operator=(const GlobalHotKey&) = delete;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
