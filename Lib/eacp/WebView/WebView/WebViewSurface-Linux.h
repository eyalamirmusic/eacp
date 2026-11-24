#pragma once

#include "WpeFrame-Linux.h"

#include <eacp/Core/Utils/Pimpl.h>
#include <eacp/GPU/View/GPUView.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace eacp::Graphics::detail
{
// What a presented frame left in the swapchain image, read back on request:
// the drawable's size and its centre pixel as B, G, R, A.
struct PresentedPixels
{
    int width = 0;
    int height = 0;
    std::array<std::uint8_t, 4> centre {};
};

// The child that puts a WPE web view's frames on screen: a GPUView filling its
// WebView that uploads the last frame into a texture and draws it across the
// drawable, presenting through the GPU module's swapchain on the view's
// Wayland subsurface or X11 child window. It ignores the pointer, so every
// mouse event lands on the WebView behind it, and never takes key focus.
//
// It animates only while new frames keep arriving: each one starts the
// continuous loop, and a tick with nothing new stops it again.
class WebViewSurface final : public GPU::GPUView
{
public:
    explicit WebViewSurface(const WpeFrame& frameToShow);
    ~WebViewSurface() override;

    // Whether the view has a native surface to present to now.
    bool canPresent() const;

    // A new frame is in the WpeFrame this was made with.
    void frameArrived();

    // A new frame reached the swapchain. Called on the loop turn after its
    // present, never from inside a render.
    std::function<void()> onPresented = [] {};

    // Frames with new content presented so far.
    int presentedFrames() const;

    // For the tests: the next frame presented is read back into
    // lastPresentedPixels(), which is empty until then.
    void readBackNextFrame();
    bool isReadBackPending() const;
    std::optional<PresentedPixels> lastPresentedPixels() const;

    void render(GPU::Frame& frame) override;
    void update(Threads::FrameTime time) override;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Graphics::detail
