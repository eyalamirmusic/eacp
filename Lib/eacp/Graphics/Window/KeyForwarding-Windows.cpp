#include "KeyForwarding-Windows.h"
#include "CompositionHostWindow-Windows.h"

namespace eacp::Graphics
{

uint16_t keyCodeFromVirtualKey(int vk);

namespace
{
HWND embedderOf(View& view)
{
    auto host = findHostHwndForView(&view);

    if (host == nullptr || (GetWindowLongPtrW(host, GWL_STYLE) & WS_CHILD) == 0)
        return nullptr;

    return GetParent(host);
}
} // namespace

bool isPlainKeyMessage(UINT message)
{
    return message == WM_KEYDOWN || message == WM_KEYUP;
}

NativeKeyEvent
    nativeKeyEventFrom(UINT message, WPARAM wParam, LPARAM lParam, DWORD time)
{
    auto isDown = message == WM_KEYDOWN;

    auto key = KeyEvent {};
    key.keyCode = keyCodeFromVirtualKey(static_cast<int>(wParam));
    key.type = isDown ? KeyEventType::Down : KeyEventType::Up;
    key.modifiers = Keyboard::getModifiers();
    key.isRepeat = isDown && (lParam & 0x40000000) != 0;
    key.timestamp = static_cast<double>(time) / 1000.0;

    return {.key = key,
            .nativeKey = static_cast<uint32_t>(wParam),
            .message = message,
            .wParam = static_cast<std::uintptr_t>(wParam),
            .lParam = static_cast<std::intptr_t>(lParam)};
}

void EmbedderKeyForwarder::deliver(const NativeKeyEvent& event)
{
    auto embedder = embedderOf(from);

    if (embedder == nullptr || !isPlainKeyMessage(event.message))
        return;

    PostMessageW(embedder,
                 event.message,
                 static_cast<WPARAM>(event.wParam),
                 static_cast<LPARAM>(event.lParam));
}

} // namespace eacp::Graphics
