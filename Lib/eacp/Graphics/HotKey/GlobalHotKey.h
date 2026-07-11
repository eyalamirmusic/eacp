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

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
