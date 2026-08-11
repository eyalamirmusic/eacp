#include "ScreenCapture.h"

#include <eacp/Core/Utils/Logging.h>

// A stub until Windows.Graphics.Capture is wired up: start() returns false so
// callers fall back to the Snapshot tier.

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
