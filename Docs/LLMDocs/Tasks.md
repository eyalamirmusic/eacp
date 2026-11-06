# Find by task

Read by `Scripts/generate-llm-docs.py` into the "Find by task" table of the
generated `index.md`. One row per task; the symbol is fully qualified and must
exist in the generated reference; several symbols are separated by ` + ` (the script warns when one does not).
Symbols outside `eacp::` are external and link to their own repository.

| task | symbol |
|---|---|
| Start an app and its event loop | `eacp::Apps::run` |
| Run one view as the whole app | `eacp::Graphics::runWindowedApp` |
| Write a command-line tool | `eacp::Apps::runConsole` |
| Parse command-line options | `eacp::Args::Parser` |
| Print lines to stdout or stderr | `eacp::Console::print` |
| Log a timestamped line | `eacp::LOG` |
| Format a string with placeholders | `eacp::Strings::format` |
| Run code on the main thread | `eacp::Threads::callAsync` |
| Run code on the main thread later | `eacp::Threads::callAfter` |
| Call a function periodically | `eacp::Threads::Timer` |
| Sync drawing to the display refresh | `eacp::Threads::DisplayLink` |
| Start a worker thread | `eacp::Threads::Thread` |
| Chain or await asynchronous results | `eacp::Threads::Async` |
| Read or write a whole file | `eacp::Files::readFile` |
| Walk a directory tree | `eacp::Files::forEachEntry` |
| Save app data | `eacp::FilePath::appSupportDirectory` + `eacp::Files::writeFile` |
| Watch files for changes | `eacp::Files::FileWatcher` |
| Get a self-deleting temporary file | `eacp::TemporaryFile` |
| Make an HTTP request (main-thread callback) | `eacp::HTTP::asyncRequest` |
| Make a blocking HTTP request | `eacp::HTTP::httpRequest` |
| Set HTTP timeouts | `eacp::HTTP::Request` |
| Download a file in the background | `eacp::HTTP::asyncDownload` |
| Download with progress and cancel | `eacp::HTTP::downloadFile` + `eacp::HTTP::DownloadProgress` |
| Fetch a file once, keep it | `eacp::OnlineResource` |
| Show download progress as a list | `eacp::UI::OnlineResourceMonitor` |
| WebSocket client, reconnect from onClose via callAfter | `eacp::WebSocket::Connection` |
| Serve WebSocket clients | `eacp::WebSocket::Server` |
| Open a TCP connection or listener | `eacp::TCP::Connection` |
| Launch a child process | `eacp::Processes::Process` |
| Run a command and wait | `eacp::Processes::run` |
| Stream a command's output into the UI | `eacp::Processes::BuildRunner` |
| Serialise a struct to JSON (MIRO_REFLECT) | `Miro::toJSONString` + `Miro::createFromJSONString` |
| Read or write a zip archive | `eacp::Zip::Reader` + `eacp::Zip::Writer` |
| Encode or decode Base64 | `eacp::Base64::encode` |
| Copy text to the clipboard | `eacp::Clipboard::copyText` |
| Pair a view with its window | `eacp::Graphics::ViewWindow` |
| Open a window with full options | `eacp::Graphics::Window` |
| Draw a custom view | `eacp::Graphics::View` |
| Draw 2D shapes (Apple, Windows) | `eacp::Graphics::Context` |
| Build a UI from components | `eacp::UI::ComponentHost` |
| Add a button, label or slider | `eacp::UI::Button` + `eacp::UI::Label` + `eacp::UI::Slider` |
| Add an editable text field | `eacp::UI::TextEditor` |
| List of clickable rows | `eacp::UI::ListBox` + `eacp::UI::ListBoxModel` |
| Render with the GPU in a view | `eacp::GPU::GPUView` |
| Render with the GPU in components | `eacp::UI::GPUComponent` |
| Draw text in a GPU component | `eacp::Text::TextRenderer` |
| Run a compute kernel on the GPU | `eacp::GPU::ComputeProgram` |
| Run a compute kernel on the CPU | `eacp::GPU::CpuCompute::Executor` |
| Shape and rasterize text | `eacp::Text::GlyphRasterizer` |
| Load and save images | `eacp::Graphics::Image` |
| Poll keyboard, mouse and gamepads | `eacp::Graphics::GameInput` |
| Capture frames from a camera | `eacp::Cameras::Camera` |
| Play a video file (macOS, Windows) | `eacp::Video::VideoView` + `eacp::Video::Player` + `eacp::Video::FrameStream` |
| Embed a web page | `eacp::Graphics::WebView` |
| Call C++ from a web page | `eacp::Graphics::WebViewBridge` |
| Hot-reload code while running | `eacp::Plugins::Reloadable` |
| Build a Core ML model | `eacp::ML::Graph` |
| Run an ML model | `eacp::ML::Model` |
