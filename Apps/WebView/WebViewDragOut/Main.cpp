#include "Miro/Reflection/ReflectMacro.h"
#include <eacp/WebView/WebView.h>
#include <WebResources.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>

using namespace eacp;
using namespace Graphics;

namespace fs = std::filesystem;

namespace
{
constexpr auto category = "DragOutApp";

fs::path downloads()
{
    const auto* home = std::getenv("HOME");
    return fs::path {home != nullptr ? home : ""} / "Downloads";
}

bool isMedia(const fs::path& path)
{
    static constexpr std::array exts = {
        ".wav", ".mp3", ".flac", ".m4a", ".aac", ".ogg", ".aif", ".aiff",
        ".mp4", ".mov", ".m4v",  ".webm", ".png", ".jpg", ".jpeg", ".gif",
        ".webp", ".svg"};

    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

auto mediaInDownloads()
{
    auto list = WebView::DraggableFileList {};
    auto ec = std::error_code {};

    for (const auto& entry: fs::directory_iterator(downloads(), ec))
        if (entry.is_regular_file(ec) && isMedia(entry.path()))
            list.files.push_back(
                {entry.path().string(), entry.path().filename().string()});

    std::sort(list.files.begin(), list.files.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return list;
}

// The embedded page + a `media://` scheme that streams files straight off disk,
// range-by-range -- so even huge media plays and seeks with no copy. The page
// loads a file as `media:///absolute/path`; only ~/Downloads is exposed.
auto mediaOptions()
{
    auto options = embeddedOptions(category);
    options.streamSchemes["media"] = diskByteSource({downloads().string()});
    return options;
}

} // namespace

// The entire app-specific API the page can call via window.eacp.invoke(...).
// Native drag-out (`armFileDrag`) is built into every WebViewBridge for free.
struct MediaApi
{
    WebView::DraggableFileList listFiles() const { return files; }
    WebView::DraggableFileList files = mediaInDownloads();

    MIRO_REFLECT_API(listFiles)
};

struct MyApp
{
    MyApp()
    {
        setApplicationMenuBar(buildDefaultWebViewMenuBar());
        window.setContentView(webView);
    }

    // api before transport: the bridge holds &api, so it must tear down first.
    MediaApi api;
    WebView webView {mediaOptions()};
    WebViewBridge transport {webView, api};
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
