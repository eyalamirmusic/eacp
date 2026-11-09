#pragma once

#include "../Graphics/Keyboard.h"

namespace eacp::Graphics
{

// A process-owned global/system hotkey. Construct one and keep it alive for as
// long as the key should be registered. Callbacks fire on the main thread.
class GlobalHotKey
{
public:
    GlobalHotKey(ModifierKeys modifiers, uint16_t keyCode, Callback onPressed);
    ~GlobalHotKey();

    GlobalHotKey(const GlobalHotKey&) = delete;
    GlobalHotKey& operator=(const GlobalHotKey&) = delete;

    // Whether the chord is free to bind: false when the system or another app
    // already holds it, so a GlobalHotKey for it would never fire. For a
    // settings screen to refuse such a chord as it is typed, rather than leave
    // the person wondering why their shortcut does nothing.
    //
    // Windows asks the system itself, registering the chord and releasing it
    // at once: RegisterHotKey refuses one the shell or any process holds, this
    // one's own GlobalHotKeys included, and a key with no Windows virtual key
    // is never free. macOS answers from the system's own enabled shortcuts
    // (System Settings > Keyboard > Keyboard Shortcuts), which win over any
    // app's; Carbon cannot say what other apps have registered, so those go
    // unseen there. Linux, Android and iOS have no global hotkeys to ask
    // about and always answer true.
    static bool isAvailable(ModifierKeys modifiers, uint16_t keyCode);

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
