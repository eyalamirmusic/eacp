#pragma once

#include "AppDriver.h"

#include <eacp/Core/Threads/EventLoop.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

namespace eacp::WebView::Test
{

// Video recording for WebView e2e tests is opt-in via EACP_RECORD_DIR: when
// it names a directory, TestMain runs non-headless (so the window is on
// screen to capture) and ScopedTestVideo writes one MP4 per test there.
// Unset, everything here is a no-op and tests run headless and fast.
inline std::optional<std::string> recordingDir()
{
    if (auto* dir = std::getenv("EACP_RECORD_DIR"); dir && *dir)
        return std::string {dir};

    return std::nullopt;
}

inline bool recordingEnabled()
{
    return recordingDir().has_value();
}

// While recording, hold the run loop so the window keeps rendering and the
// recorder keeps capturing — long enough for a UI step to be visible in the
// video. A no-op when not recording, so it never slows ordinary runs.
inline void settle(std::chrono::milliseconds pause = std::chrono::milliseconds {700})
{
    if (recordingEnabled())
        Threads::runEventLoopFor(pause);
}

// Records the driver's app window into <EACP_RECORD_DIR>/<name>.mp4 for its
// own lifetime, with a brief settle at each end so the opening and final
// states are visible. Drop one in at the top of a test body.
class ScopedTestVideo
{
public:
    ScopedTestVideo(AppDriver& driverToRecord, const std::string& name)
        : driver {driverToRecord}
    {
        if (auto dir = recordingDir())
            driver.startRecording(*dir + "/" + name + ".mp4");

        settle();
    }

    ~ScopedTestVideo()
    {
        settle();
        driver.stopRecording();
    }

    ScopedTestVideo(const ScopedTestVideo&) = delete;
    ScopedTestVideo& operator=(const ScopedTestVideo&) = delete;

private:
    AppDriver& driver;
};

} // namespace eacp::WebView::Test
