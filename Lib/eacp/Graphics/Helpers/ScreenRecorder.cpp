#include "ScreenRecorder.h"

// Stub implementation for platforms without a window-capture + video
// encoder backend yet (Windows, Linux, iOS). start() reports failure so
// callers can degrade gracefully; macOS has the real implementation in
// ScreenRecorder-macOS.mm.

namespace eacp::Graphics
{

struct ScreenRecorder::Native
{
    bool recording = false;
};

ScreenRecorder::ScreenRecorder() = default;
ScreenRecorder::~ScreenRecorder() = default;

bool ScreenRecorder::isRecording() const
{
    return false;
}

bool ScreenRecorder::start(Window&, const std::string&, Options, std::string* error)
{
    if (error)
        *error = "ScreenRecorder: screen recording is only supported on macOS";
    return false;
}

bool ScreenRecorder::start(View&, const std::string&, Options, std::string* error)
{
    if (error)
        *error = "ScreenRecorder: screen recording is only supported on macOS";
    return false;
}

std::string ScreenRecorder::stop()
{
    return {};
}

bool ScreenRecorder::startWindow(void*, const std::string&, Options, std::string*)
{
    return false;
}

Image captureWindowImage(Window&, std::string* error)
{
    if (error)
        *error = "ScreenRecorder: window image capture is only supported on macOS";
    return {};
}

Image captureViewImage(View&, std::string* error)
{
    if (error)
        *error = "ScreenRecorder: window image capture is only supported on macOS";
    return {};
}

} // namespace eacp::Graphics
