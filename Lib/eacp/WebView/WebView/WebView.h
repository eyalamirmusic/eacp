#pragma once

#include "../Common.h"

namespace eacp::Graphics
{
using Bytes = Vector<std::uint8_t>;
using ByteSpan = std::span<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

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

// Fills `out` from `offset`, returning bytes written (0 == end of resource).
// Called repeatedly with advancing offsets, possibly off the main thread.
using ResourceReader = std::function<std::size_t(RangeSize offset, ByteSpan out)>;

// A resource served in chunks. Report the MIME type and the full `size`; the
// handler does the Range parsing and 200/206/416 header work itself.
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

// `scheme://host/abs/path?query#frag` -> `/abs/path`, percent-decoded and
// still absolute (unlike pathFromURL). Empty if the URL has no path.
std::string fileURLToPath(std::string_view url);

FileProvider fromResEmbed(std::string category);

// Serves files off disk for a custom scheme, in bounded chunks with Range
// support. Requests resolving outside every root 404; an empty `roots` allows
// any readable file. MIME defaults to mimeForPath.
StreamingProvider fileStreamProvider(
    Vector<std::string> roots,
    std::function<std::string(std::string_view path)> mimeForFile = {});

struct WebViewNativeAccess;

class WebView : public View
{
public:
    // `path` is absolute and on-disk; `name` is only the display label.
    struct DraggableFile
    {
        std::string path;
        std::string name;

        MIRO_REFLECT(path, name)
    };

    // Payload of the built-in `armFileDrag` bridge command; several files start
    // one multi-file drag session.
    struct DraggableFileList
    {
        Vector<DraggableFile> files;

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
        bool transparentBackground = false;

        // Windows only: WebView2 requires every environment sharing a
        // user-data-folder to register the SAME custom schemes, so two WebViews
        // in one exe with different `schemes` need distinct suffixes here.
        std::string userDataFolderSuffix;

        // Windows: WebView2's hover-link URL overlay. Off so the embedding
        // matches macOS, where WKWebView has no status bar.
        bool statusBar = false;

        // macOS NSView acceptsFirstMouse: deliver the click that activates an
        // unfocused window to the page too. Opt-in, as it allows accidental
        // first-click interaction. Windows clicks already reach the page.
        bool acceptFirstMouse = false;

        // Fire onUnhandledKeyEvent for keys the page did not consume, then pass
        // them to whatever hosts the view. The page's verdict arrives a few ms
        // late. WebView2 keeps its browser shortcuts; no-op on iOS.
        bool forwardUnhandledKeys = false;

        // An off-screen WKWebView has no display link, so requestAnimationFrame
        // stops and snapshots freeze. Redirects rAF onto a ~60 Hz timer for the
        // view's whole life — for snapshot hosts, not editors. No-op on Windows.
        bool driveOffscreenAnimation = false;
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
                            const JSCallback& callback = nullptr);

    // Rejects with the error message if the script threw. Resolve and reject
    // both fire on the main thread.
    Threads::Async<std::string> callJS(const std::string& script);

    using SnapshotCallback =
        std::function<void(Bytes pngBytes, const std::string& error)>;
    void takeSnapshot(SnapshotCallback callback);

    // The page can only be captured asynchronously, so the sync renderToImage
    // leaves the web region blank.
    bool hasAsyncContent() const override { return true; }
    void captureAsyncContent(float scale, std::function<void(Image)> done) override;

    void zoomIn();
    void zoomOut();
    void resetZoom();
    void setZoom(double level);
    double getZoom() const;

    // Focuses the browser runtime itself, not just the wrapper View.
    void focusContent();

    static WebView* focused();

    // Always true on macOS/iOS; on Windows, probes for the Edge WebView2
    // Runtime. Check before constructing, or environment creation fails
    // silently later.
    static bool isRuntimeAvailable();

    void addScriptMessageHandler(
        const std::string& name,
        std::function<void(const std::string& message)> handler);
    void removeScriptMessageHandler(const std::string& name);

    void addUserScript(const std::string& source, bool atDocumentStart = true);

    // Arms the next mouse gesture as an OS drag-out, so it can escape the app.
    // Prefer the built-in `armFileDrag` bridge command. Asserts on iOS.
    void armFileDrag(const Vector<std::string>& paths);

    // Arms the next mouse gesture as a window drag. Asserts on iOS.
    void armWindowDrag();

    std::function<void(const std::string& url)> onNavigationStarted = [](auto&&) {};
    std::function<void(const std::string& url)> onNavigationFinished = [](auto&&) {};
    std::function<void(const std::string& error)> onNavigationFailed = [](auto&&) {};
    std::function<void(const std::string& title)> onTitleChanged = [](auto&&) {};

    std::function<bool(OwningPointer<WebView> popup, const std::string& url)>
        onNewWindowRequested = [](auto&&, auto&&) { return false; };

    // Cursor during a file drag-out, in client CSS pixels (the clientX/clientY
    // space). `inside` goes false once the pointer leaves the window.
    struct FileDragPoint
    {
        double x = 0;
        double y = 0;
        bool inside = false;
    };

    std::function<void()> onFileDragStarted = [] {};
    // Windows only, where the OS file drag is a blocking modal loop. macOS
    // drags are async and the host polls the cursor itself.
    std::function<void(FileDragPoint)> onFileDragMoved = [](auto&&) {};
    std::function<void(FileDragPoint)> onFileDragEnded = [](auto&&) {};
    std::function<void()> onClose = [] {};

    // Needs Options::forwardUnhandledKeys. Return true to consume the event;
    // false sends it on up the native responder chain.
    std::function<bool(const KeyEvent&)> onUnhandledKeyEvent;

    struct Native;

protected:
    void resized() override;

    // The platform web view itself, so focus lands on the live page rather than
    // the container View hosting it.
    void* nativeFocusTarget() override;

    // Windows places the browser's input/IME surface in screen coordinates,
    // outside the composition tree, so it must follow the window explicitly.
    void hostWindowMoved() override;
    void hostWindowVisibilityChanged(bool visible) override;

    // Windows hosts the WebView as a composition visual with no input HWND, so
    // routed mouse events are forwarded to the browser here. No-ops elsewhere.
    void mouseDown(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseDragged(const MouseEvent&) override;
    void mouseMoved(const MouseEvent&) override;
    void mouseExited(const MouseEvent&) override;
    void mouseWheel(const MouseEvent&) override;

private:
    friend struct WebViewNativeAccess;

    struct PopupInit;
    explicit WebView(PopupInit init);
    void initNative(Options options);
    void installWindowDragSupport();
    void installWindowControlSupport();

    void installOffscreenAnimationSupport();
    void installKeyEventSupport();
    void performWindowControl(const std::string& action);
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
