#include <eacp/WebView/WebView.h>
#include <eacp/Core/Utils/StdPath.h>
#include <WebResources.h>
#include <algorithm>

#include <array>
#include <cstdlib>
#include <fstream>
#include <vector>

using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto category = "DragOutApp";

// The custom URL scheme this app serves its on-disk files over. The page
// references files as `audiofile:///abs/path.wav`; the app registers and owns
// the provider for it below (see diskFileProvider / dragOutOptions).
constexpr auto audioScheme = "audiofile";

constexpr std::array audioExtensions = {
    ".wav", ".mp3", ".aif", ".aiff", ".flac", ".m4a", ".aac", ".ogg"};

std::string lowerExtension(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    std::transform(ext.begin(),
                   ext.end(),
                   ext.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool isAudioFile(const std::filesystem::path& path)
{
    auto ext = lowerExtension(path);
    return std::find(audioExtensions.begin(), audioExtensions.end(), ext)
           != audioExtensions.end();
}

std::filesystem::path downloadsDir()
{
    return toStdPath(FilePath::downloadsDirectory());
}

std::filesystem::path bundledAssetDir()
{
    return std::filesystem::temp_directory_path() / "eacp-dragout";
}

// Embedded app resources + the `audiofile` scheme that streams the listed
// files off disk into the page's inline players, in bounded chunks with Range
// support (fileStreamProvider does the disk reading, MIME, and sandboxing).
// The roots bound which directories the page may read: only ~/Downloads and
// the extracted bundled assets, nothing else on disk.
WebView::Options dragOutOptions()
{
    auto options = embeddedOptions(category);
    options.streamingSchemes[audioScheme] =
        fileStreamProvider({downloadsDir().string(), bundledAssetDir().string()});
    return options;
}

// Materialise an embedded resource to a temp file so it has a real path the OS
// can copy when dragged out. Returns the absolute path, or empty if missing.
std::string extractBundledAsset(const std::string& name)
{
    auto asset = ResEmbed::get(name, category);

    if (!asset)
        return {};

    auto ec = std::error_code {};
    auto dir = bundledAssetDir();
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
        setApplicationMenuBar(buildDefaultWebViewMenuBar(), window);
        window.setContentView(webView);
    }

    // api declared first -> destructed last (after the bridge tears down its
    // handlers/listeners, which hold &api).
    DragOutApi api;
    WebView webView {dragOutOptions()};
    WebViewBridge transport {webView, api};
    Window window;
};

int main()
{
    return eacp::Apps::run<MyApp>();
}
