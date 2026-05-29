#pragma once

#include <eacp/Core/Threads/Async.h>
#include <eacp/Graphics/Graphics.h>
#include <Miro/Miro.h>
#include <ResEmbed/ResEmbed.h>
#include <ea_data_structures/Pointers/OwningPointer.h>
#include <ea_data_structures/Structures/Vector.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eacp::Graphics
{
struct ResourceResponse
{
    std::string mimeType;
    EA::Vector<std::uint8_t> data;
    int statusCode = 200;
};

using ResourceProvider =
    std::function<std::optional<ResourceResponse>(std::string_view url)>;

using FileProvider = std::function<std::optional<std::span<const std::uint8_t>>(
    std::string_view path)>;

std::string mimeForPath(std::string_view path);

std::string pathFromURL(std::string_view url,
                        std::string_view indexFile = "index.html");

FileProvider fromResEmbed(std::string category);

// Maps a request URL to the absolute on-disk path it should serve, or
// nullopt to reject (404). Used by the streaming file-scheme handler, which
// reads only the requested byte range off the main thread -- so a large
// media file neither blocks the UI nor gets re-read whole on every seek.
using FilePathResolver =
    std::function<std::optional<std::string>(std::string_view url)>;

// A FilePathResolver for the disk-file scheme. The URL maps to an absolute
// path: `scheme:///abs/path.wav` -> /abs/path.wav, percent-decoded so spaces
// and unicode survive. If `allowedRoots` is non-empty, only existing files
// under one of those directories resolve (anything else 404s) -- recommended,
// since this hands disk reads to web content. An empty list serves any
// absolute path the page asks for.
FilePathResolver diskFileResolver(std::vector<std::string> allowedRoots = {});

struct WebViewNativeAccess;

class WebView : public View
{
public:
    // A single file the page can hand to the native drag-out. `path` is the
    // absolute on-disk path the OS copies on drop; `name` is the display label.
    struct DraggableFile
    {
        std::string path;
        std::string name;

        MIRO_REFLECT(path, name)
    };

    // Payload of the built-in `armFileDrag` bridge command. Serializable via
    // Miro, so the page sends `{ files: [{ path, name }, ...] }` and the bridge
    // deserializes it straight into this type -- no hand-rolled JSON on either
    // side. Multiple files start a single multi-file drag session.
    struct DraggableFileList
    {
        std::vector<DraggableFile> files;

        MIRO_REFLECT(files)
    };

    struct Options
    {
        struct Embedded
        {
            bool enabled = false;
            FileProvider provider;
            std::string scheme = "app";
            std::string host = "local";
            std::string indexFile = "index.html";
            std::string devServerURL = "http://localhost:5173";
            bool preferDevServer = true;
            int devServerProbeTimeoutMs = 150;
            bool autoLoad = true;
        };

        std::unordered_map<std::string, ResourceProvider> schemes;

        // Schemes served straight from disk. Unlike `schemes` (which hand
        // back a whole in-memory buffer), each request only reads the bytes
        // the page asked for, off the main thread -- so large media files
        // stream and seek without blocking the UI or being re-read whole.
        std::unordered_map<std::string, FilePathResolver> fileSchemes;

        Embedded embedded;
        bool debugConsole = true;
    };

    WebView();
    explicit WebView(Options options);
    ~WebView() override;

    void loadURL(const std::string& url);
    void loadHTML(const std::string& html, const std::string& baseURL = "");

    void goBack();
    void goForward();
    void reload();
    void stopLoading();

    bool canGoBack() const;
    bool canGoForward() const;
    bool isLoading() const;

    std::string getURL() const;
    std::string getTitle() const;

    using JSCallback =
        std::function<void(const std::string& result, const std::string& error)>;
    void evaluateJavaScript(const std::string& script,
                            JSCallback callback = nullptr);

    // Awaitable wrapper around evaluateJavaScript. The returned Async
    // resolves with the script result string, or rejects with the
    // error message if the script threw. Both resolve and reject fire
    // on the main thread.
    Threads::Async<std::string> callJS(const std::string& script);

    using SnapshotCallback = std::function<void(std::vector<std::uint8_t> pngBytes,
                                                const std::string& error)>;
    void takeSnapshot(SnapshotCallback callback);

    void zoomIn();
    void zoomOut();
    void resetZoom();
    void setZoom(double level);
    double getZoom() const;

    static WebView* focused();

    void addScriptMessageHandler(
        const std::string& name,
        std::function<void(const std::string& message)> handler);
    void removeScriptMessageHandler(const std::string& name);

    void addUserScript(const std::string& source, bool atDocumentStart = true);

    // Arms a native file drag-out of the given on-disk files for the next mouse
    // gesture. The drag is started from the real mouseDragged: event once the
    // pointer crosses the drag threshold, so it escapes the app into Finder /
    // a DAW (a session started from an async callback cannot). Prefer the
    // built-in `armFileDrag` bridge command, which deserializes an
    // eacp::WebView::DraggableFileList and routes here. macOS-only; a no-op on
    // other platforms.
    void armFileDrag(const std::vector<std::string>& paths);

    std::function<void(const std::string& url)> onNavigationStarted = [](auto&&) {};
    std::function<void(const std::string& url)> onNavigationFinished = [](auto&&) {};
    std::function<void(const std::string& error)> onNavigationFailed = [](auto&&) {};
    std::function<void(const std::string& title)> onTitleChanged = [](auto&&) {};

    std::function<bool(EA::OwningPointer<WebView> popup, const std::string& url)>
        onNewWindowRequested = [](auto&&, auto&&) { return false; };

    std::function<void()> onClose = [] {};

    struct Native;

protected:
    void resized() override;

private:
    friend struct WebViewNativeAccess;

    struct PopupInit;
    explicit WebView(PopupInit init);
    void initNative(Options options);
    std::shared_ptr<Native> impl;
};

inline WebView::Options embeddedOptions(std::string category)
{
    auto options = WebView::Options {};
    options.embedded.enabled = true;
    options.embedded.provider = fromResEmbed(std::move(category));
    return options;
}

// Default scheme for the built-in disk-file streamer. The page references
// files as `audiofile:///abs/path.wav`.
inline constexpr auto diskFileScheme = "audiofile";

// Registers the built-in disk-file streamer on `options` under `scheme`, so
// the page can load on-disk files (e.g. play them in an <audio> element) via
// `audiofile:///abs/path`. Requests are served range-by-range off the main
// thread, so large files stream and seek smoothly. `allowedRoots` bounds
// which directories are readable -- see diskFileResolver.
inline void enableDiskFiles(WebView::Options& options,
                            std::vector<std::string> allowedRoots = {},
                            std::string scheme = diskFileScheme)
{
    options.fileSchemes[std::move(scheme)] =
        diskFileResolver(std::move(allowedRoots));
}
} // namespace eacp::Graphics
