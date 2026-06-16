#pragma once

#include <eacp/Core/Utils/Pimpl.h>
#include <eacp/Graphics/Image/Image.h>

#include <string>

namespace eacp::Graphics
{

class Window;
class View;

struct WindowRecorderOptions
{
    // Captured/encoded frames per second.
    int frameRateHz = 30;
};

// Records a single window's on-screen content to an H.264 MP4. It captures
// the *composited* window from the window server — whatever the window shows
// (WebView, GPU, native), all at once — not the whole screen and not any
// other window. It is a standalone object that takes the Window it records,
// so only apps that want recording pay for it; ScopedWindowRecorder ties a
// recording to an owner's lifetime.
//
// macOS only; start() returns false with an error elsewhere. The window
// must be visible, and recent macOS needs Screen Recording permission.
class WindowRecorder
{
public:
    using Options = WindowRecorderOptions;

    WindowRecorder();
    ~WindowRecorder();

    WindowRecorder(const WindowRecorder&) = delete;
    WindowRecorder& operator=(const WindowRecorder&) = delete;

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

// RAII window recording: starts recording `window` to `path` on
// construction and stops (flushing the MP4) on destruction. A drop-in
// member for an app that wants its whole run captured — declare it after
// the window it records so the window is already on-screen when recording
// begins. A failed start (off-screen window, missing permission, non-macOS)
// is silent; query isRecording() if you need to know.
class ScopedWindowRecorder
{
public:
    ScopedWindowRecorder(Window& window,
                         const std::string& path,
                         WindowRecorderOptions options = {})
    {
        recorder.start(window, path, options);
    }

    ~ScopedWindowRecorder() { recorder.stop(); }

    ScopedWindowRecorder(const ScopedWindowRecorder&) = delete;
    ScopedWindowRecorder& operator=(const ScopedWindowRecorder&) = delete;

    bool isRecording() const { return recorder.isRecording(); }

private:
    WindowRecorder recorder;
};

// One-shot capture of the *composited* window image — same source as the
// recorder. Returns an invalid Image (operator bool == false) and sets
// *error on failure. Window::captureImage() wraps this. macOS only.
Image captureWindowImage(Window& window, std::string* error = nullptr);
Image captureViewImage(View& view, std::string* error = nullptr);

} // namespace eacp::Graphics
