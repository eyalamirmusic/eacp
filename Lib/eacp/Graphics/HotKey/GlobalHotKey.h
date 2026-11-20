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

    // False when something holds the chord, so a GlobalHotKey on it never fires.
    // macOS sees only the system's own shortcuts, not other apps' chords.
    static bool isAvailable(ModifierKeys modifiers, uint16_t keyCode);

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
