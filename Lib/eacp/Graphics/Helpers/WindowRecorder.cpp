#include "WindowRecorder.h"

// Stub implementation for platforms without a window-capture + video
// encoder backend yet (Windows, Linux, iOS). start() reports failure so
// callers can degrade gracefully; macOS has the real implementation in
// WindowRecorder-macOS.mm.

namespace eacp::Graphics
{

struct WindowRecorder::Native
{
    bool recording = false;
};

WindowRecorder::WindowRecorder() = default;
WindowRecorder::~WindowRecorder() = default;

bool WindowRecorder::isRecording() const
{
    return false;
}

bool WindowRecorder::start(Window&, const std::string&, Options, std::string* error)
{
    if (error)
        *error = "WindowRecorder: window recording is only supported on macOS";
    return false;
}

bool WindowRecorder::start(View&, const std::string&, Options, std::string* error)
{
    if (error)
        *error = "WindowRecorder: window recording is only supported on macOS";
    return false;
}

std::string WindowRecorder::stop()
{
    return {};
}

bool WindowRecorder::startWindow(void*, const std::string&, Options, std::string*)
{
    return false;
}

Image captureWindowImage(Window&, std::string* error)
{
    if (error)
        *error = "WindowRecorder: window image capture is only supported on macOS";
    return {};
}

Image captureViewImage(View&, std::string* error)
{
    if (error)
        *error = "WindowRecorder: window image capture is only supported on macOS";
    return {};
}

} // namespace eacp::Graphics
