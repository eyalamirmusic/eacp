#include <eacp/WebView/WebView.h>
#include <ResEmbed/ResEmbed.h>
#include <WebResources.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto category = "DragOutApp";

constexpr std::array audioExtensions = {
    ".wav", ".mp3", ".aif", ".aiff", ".flac", ".m4a", ".aac", ".ogg"};

bool isAudioFile(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    std::transform(ext.begin(),
                   ext.end(),
                   ext.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return std::find(audioExtensions.begin(), audioExtensions.end(), ext)
           != audioExtensions.end();
}

// The user's home directory. Windows exposes it as USERPROFILE (HOME is
// usually unset there) and steers CRT callers off the deprecated getenv
// toward _dupenv_s; macOS and Linux use HOME.
std::filesystem::path homeDirectory()
{
#ifdef _WIN32
    char* value = nullptr;
    auto size = std::size_t {0};
    if (_dupenv_s(&value, &size, "USERPROFILE") != 0 || value == nullptr)
        return {};

    auto home = std::filesystem::path {value};
    std::free(value);
    return home;
#else
    const auto* home = std::getenv("HOME");
    return std::filesystem::path {home != nullptr ? home : ""};
#endif
}

std::filesystem::path downloadsDir()
{
    return homeDirectory() / "Downloads";
}

// Materialise an embedded resource to a temp file so it has a real path the OS
// can copy when dragged out. Returns the absolute path, or empty if missing.
std::string extractBundledAsset(const std::string& name)
{
    auto asset = ResEmbed::get(name, category);

    if (!asset)
        return {};

    auto ec = std::error_code {};
    auto dir = std::filesystem::temp_directory_path() / "eacp-dragout";
    std::filesystem::create_directories(dir, ec);

    auto path = dir / name;
    auto out = std::ofstream {path, std::ios::binary};
    out.write(asset.asCharPointer(), static_cast<std::streamsize>(asset.size()));

    return path.string();
}

WebView::DraggableFileList buildFileList()
{
    auto list = WebView::DraggableFileList {};

    // Bundled ResEmbed assets first, to show embedded resources drag out too.
    for (const auto* name: {"sample.png", "sample.mp3"})
    {
        auto path = extractBundledAsset(name);
        if (!path.empty())
            list.files.push_back({path, name});
    }

    // Then real audio files from ~/Downloads.
    auto ec = std::error_code {};
    auto downloads = std::vector<std::string> {};

    for (const auto& entry: std::filesystem::directory_iterator(downloadsDir(), ec))
    {
        if (entry.is_regular_file(ec) && isAudioFile(entry.path()))
            downloads.push_back(entry.path().string());
    }

    std::sort(downloads.begin(), downloads.end());

    for (const auto& path: downloads)
        list.files.push_back(
            {path, std::filesystem::path {path}.filename().string()});

    return list;
}
} // namespace

// App API exposed over the bridge. `listFiles` is the only app-specific
// command; `armFileDrag` is a built-in the WebViewBridge registers for every
// app. The page invokes both via window.eacp.invoke(...).
class DragOutApi
{
public:
    DragOutApi()
        : fileList(buildFileList())
    {
    }

    void reflect(Miro::ApiReflector& r) { r.commands<&DragOutApi::listFiles>(); }

    WebView::DraggableFileList listFiles() const { return fileList; }

private:
    WebView::DraggableFileList fileList;
};

struct MyApp
{
    MyApp()
    {
        setApplicationMenuBar(buildDefaultWebViewMenuBar());
        window.setContentView(webView);
    }

    // api declared first -> destructed last (after the bridge tears down its
    // handlers/listeners, which hold &api).
    DragOutApi api;
    WebView webView {embeddedOptions(category)};
    WebViewBridge transport {webView, api};
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
