#include "ScreenCapture.h"

#include <eacp/Core/Utils/Logging.h>

// The Screen tier on Windows will tap the compositor via Windows.Graphics.Capture
// (GraphicsCaptureItem from the host HWND -> Direct3D11CaptureFramePool -> encoder).
// Until that lands, start() returns false so callers fall back to the Snapshot
// tier, mirroring how the macOS path fails when Screen Recording is unavailable.

namespace eacp::Video
{
namespace
{
struct WindowsScreenCapture final : ScreenCapture
{
    bool start(Graphics::View&,
               const FilePath&,
               const VideoOptions&,
               Encoder&) override
    {
        LOG("VideoRecorder: Screen capture (Windows.Graphics.Capture) not yet "
            "implemented; use CaptureMode::Snapshot");
        return false;
    }

    Threads::Async<void> stop() override
    {
        auto promise = Threads::AsyncPromise<void> {};
        promise.resolve();
        return promise.get();
    }
};
} // namespace

OwningPointer<ScreenCapture> makeScreenCapture()
{
    return makeOwned<WindowsScreenCapture>();
}

} // namespace eacp::Video
