#include "ScreenCapture.h"

#include <eacp/Core/Threads/EventLoop.h>
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
               const RecordingOptions&,
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

// Windows.Graphics.Capture needs no user consent to capture the app's own
// window -- the picker exists for capturing somebody else's -- so there is
// nothing to ask for and nothing that can refuse.
bool hasScreenCapturePermission()
{
    return true;
}

void requestScreenCapturePermission(std::function<void(bool)> onResult)
{
    Threads::callAsync(
        [onResult]
        {
            if (onResult)
                onResult(true);
        });
}

OwningPointer<ScreenCapture> makeScreenCapture()
{
    return makeOwned<WindowsScreenCapture>();
}

} // namespace eacp::Video
