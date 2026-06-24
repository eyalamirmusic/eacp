#pragma once

#include <eacp/Core/Utils/Pimpl.h>
#include <eacp/Graphics/Image/Image.h>

#include <string>

namespace eacp::Graphics
{

class Window;
class View;

struct ScreenRecorderOptions
{
    // Captured/encoded frames per second.
    int frameRateHz = 30;
};

// Records a window's on-screen content to an H.264 MP4. It captures the
// *composited* window from the window server — whatever the window shows
// (WebView, GPU, native), all at once — and depends on neither the WebView
// nor the debug server. Window::startScreenRecording() wraps it.
//
// macOS only; start() returns false with an error elsewhere. The window
// must be visible, and recent macOS needs Screen Recording permission.
class ScreenRecorder
{
public:
    using Options = ScreenRecorderOptions;

    ScreenRecorder();
    ~ScreenRecorder();

    ScreenRecorder(const ScreenRecorder&) = delete;
    ScreenRecorder& operator=(const ScreenRecorder&) = delete;

    // Begin recording `window` (or the window containing `view`) to
    // `path` as MP4. Returns false and sets *error on failure — already
    // recording, no on-screen window, missing permission, or an encoder
    // error. Parent directories of `path` are created.
    bool start(Window& window,
               const std::string& path,
               Options options = {},
               std::string* error = nullptr);
    bool start(View& view,
               const std::string& path,
               Options options = {},
               std::string* error = nullptr);

    // Finish the file and stop capturing. Blocks until the MP4 is
    // flushed to disk. Returns the output path, or an empty string if
    // nothing was recording.
    std::string stop();

    bool isRecording() const;

private:
    bool startWindow(void* nativeWindow,
                     const std::string& path,
                     Options options,
                     std::string* error);

    struct Native;
    Pimpl<Native> impl;
};

// One-shot capture of the *composited* window image — same source as the
// recorder. Returns an invalid Image (operator bool == false) and sets
// *error on failure. Window::captureImage() wraps this. macOS only.
Image captureWindowImage(Window& window, std::string* error = nullptr);
Image captureViewImage(View& view, std::string* error = nullptr);

} // namespace eacp::Graphics
