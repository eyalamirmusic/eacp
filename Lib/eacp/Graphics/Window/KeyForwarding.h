#pragma once

#include "../Graphics/Keyboard.h"

#include <cstdint>

namespace eacp::Graphics
{
class View;

struct NativeKeyEvent
{
    KeyEvent key;
    uint32_t nativeKey = 0;
    void* nsEvent = nullptr;
    unsigned int message = 0;
    std::uintptr_t wParam = 0;
    std::intptr_t lParam = 0;
};

class EmbedderKeyForwarder
{
public:
    using Claim = std::function<bool(const KeyEvent&)>;

    explicit EmbedderKeyForwarder(View& fromToUse);

    void forward(const NativeKeyEvent& event);

    // An echo never reaches `claim`, and a claimed key still counts as sent.
    void forwardUnpaired(const NativeKeyEvent& event, const Claim& claim);

private:
    bool pairsWithForwardedDown(const NativeKeyEvent& event);
    void deliver(const NativeKeyEvent& event);

    View& from;
    Vector<uint32_t> forwardedDowns;
};

} // namespace eacp::Graphics
