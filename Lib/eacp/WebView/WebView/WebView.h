#pragma once

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Utils/Range.h>
#include <eacp/Graphics/Graphics.h>
#include <Miro/Miro.h>
#include <ResEmbed/ResEmbed.h>
#include <ea_data_structures/Pointers/OwningPointer.h>
#include <ea_data_structures/Structures/Vector.h>
#include <cstddef>
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
// Owning byte buffer and non-owning views used across the resource API.
using Bytes = EA::Vector<std::uint8_t>;
using ByteSpan = std::span<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// Byte offsets / lengths into a resource, and a half-open byte range.
using RangeSize = std::uint64_t;
using ByteRange = Range<RangeSize>;

struct ResourceResponse
{
    std::string mimeType;
    Bytes data;
    int statusCode = 200;
};

using ResourceProvider =
    std::function<std::optional<ResourceResponse>(std::string_view url)>;

using FileProvider = std::function<std::optional<ByteView>(std::string_view path)>;

// Sequential pull reader for a streamed resource: fill `out` starting at byte
// `offset`, returning the number of bytes written (0 == end of resource). The
// handler calls it repeatedly with monotonically advancing offsets, and may
// call it off the main thread -- so it must be safe to run on a background
// queue.
using ResourceReader = std::function<std::size_t(RangeSize offset, ByteSpan out)>;

// A resource served in chunks rather than as one in-memory blob. The provider
// reports the MIME type and the full `size`; the handler owns Range parsing,
// emits 200 / 206 / 416 with the right Content-Range / Content-Length /
// Accept-Ranges headers, and pulls the body through `read` as it goes.
struct StreamingResource
{
    std::string mimeType;
    RangeSize size = 0;
    ResourceReader read;
    int statusCode = 200;
};

using StreamingProvider =
    std::function<std::optional<StreamingResource>(std::string_view url)>;

std::string mimeForPath(std::string_view path);

std::string pathFromURL(std::string_view url,
                        std::string_view indexFile = "index.html");

// `scheme://host/abs/path?query#frag` -> `/abs/path`, percent-decoded. Unlike
// pathFromURL (which yields a host-relative resource key for embedded schemes),
// this keeps the leading slash so the result is an absolute filesystem path.
// Empty if the URL has no path.
std::string fileURLToPath(std::string_view url);

FileProvider fromResEmbed(std::string category);

// A StreamingProvider that serves files straight off disk for a custom scheme,
// in bounded chunks with Range support. `roots` bounds which directories are
// readable: a request resolving outside every root is rejected (404), and an
// empty `roots` allows any readable file. MIME defaults to mimeForPath; pass
// `mimeForFile` to override. Pair with Options::streamingSchemes.
StreamingProvider fileStreamProvider(
    EA::Vector<std::string> roots,
    std::function<std::string(std::string_view path)> mimeForFile = {});

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
        std::unordered_map<std::string, StreamingProvider> streamingSchemes;
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

    // Arms a native window drag for the next mouse gesture. Driven by the
    // injected window-drag script, not called directly. Implemented on macOS;
    // asserts on backends without window-drag support.
    void armWindowDrag();

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
    void installWindowDragSupport(); // defined in WebView-Shared.cpp
    std::shared_ptr<Native> impl;
};

inline WebView::Options embeddedOptions(std::string category)
{
    auto options = WebView::Options {};
    options.embedded.enabled = true;
    options.embedded.provider = fromResEmbed(std::move(category));
    return options;
}
} // namespace eacp::Graphics
