#include <eacp/WebView/WebView.h>
#include <eacp/Core/Utils/File.h>
#include <ResEmbed/ResEmbed.h>
#include <WebResources.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

std::filesystem::path bundledAssetDir()
{
    return std::filesystem::temp_directory_path() / "eacp-dragout";
}

std::string mimeForFile(const std::filesystem::path& path)
{
    auto ext = lowerExtension(path);

    if (ext == ".mp3")
        return "audio/mpeg";
    if (ext == ".wav")
        return "audio/wav";
    if (ext == ".aif" || ext == ".aiff")
        return "audio/aiff";
    if (ext == ".flac")
        return "audio/flac";
    if (ext == ".m4a" || ext == ".mp4")
        return "audio/mp4";
    if (ext == ".aac")
        return "audio/aac";
    if (ext == ".ogg" || ext == ".opus")
        return "audio/ogg";
    if (ext == ".png")
        return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")
        return "image/jpeg";
    if (ext == ".gif")
        return "image/gif";
    if (ext == ".webp")
        return "image/webp";
    if (ext == ".svg")
        return "image/svg+xml";
    return "application/octet-stream";
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string percentDecode(std::string_view encoded)
{
    auto out = std::string {};
    out.reserve(encoded.size());

    for (auto i = std::size_t {0}; i < encoded.size(); ++i)
    {
        if (encoded[i] == '%' && i + 2 < encoded.size())
        {
            auto hi = hexDigit(encoded[i + 1]);
            auto lo = hexDigit(encoded[i + 2]);

            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }

        out.push_back(encoded[i]);
    }

    return out;
}

// `audiofile:///abs/path?query#frag` -> `/abs/path`, percent-decoded. The
// host segment (between `://` and the next `/`) is ignored, so an empty host
// (`audiofile:///`) yields a leading-slash absolute path as-is.
std::string pathFromFileURL(std::string_view url)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string_view::npos)
        return {};

    auto rest = url.substr(schemeEnd + 3);
    auto cut = rest.find_first_of("?#");

    if (cut != std::string_view::npos)
        rest = rest.substr(0, cut);

    auto slash = rest.find('/');

    if (slash == std::string_view::npos)
        return {};

    return percentDecode(rest.substr(slash));
}

bool isUnderRoot(const std::filesystem::path& file,
                 const std::filesystem::path& root)
{
    auto ec = std::error_code {};
    auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    auto rel = std::filesystem::relative(file, canonicalRoot, ec);

    if (ec || rel.empty())
        return false;

    // A path that escapes the root resolves to a relative path starting
    // with "..". Anything else (including ".") is contained.
    return rel.native().rfind("..", 0) != 0;
}

// Streams file bytes off disk for the `audiofile` scheme, so the page can
// play/preview them in inline <audio>/<img> elements without ever loading a
// whole file into memory. The roots bound which directories are readable --
// anything outside 404s. The handler pulls the body through File::read in
// bounded chunks and honours the browser's byte-range requests (206) so media
// can seek.
StreamingProvider diskFileProvider(std::vector<std::string> allowedRoots)
{
    return [roots = std::move(allowedRoots)](
               std::string_view url) -> std::optional<StreamingResource>
    {
        auto pathStr = pathFromFileURL(url);

        if (pathStr.empty())
            return std::nullopt;

        auto ec = std::error_code {};
        auto path = std::filesystem::weakly_canonical(pathStr, ec);

        if (ec)
            path = std::filesystem::path {pathStr};

        auto allowed = roots.empty()
                       || std::any_of(roots.begin(),
                                      roots.end(),
                                      [&](const auto& root)
                                      { return isUnderRoot(path, root); });

        // Kept open behind a shared_ptr so the reader can pull chunks across
        // many scheme-task callbacks, then closes when the last reader drops.
        auto file = std::make_shared<eacp::File>(path);

        if (!allowed || !file->isRegularFile() || !file->openForRead())
            return std::nullopt;

        auto response = StreamingResource {};
        response.mimeType = mimeForFile(path);
        response.size = file->size();
        response.read = [file](RangeSize offset, ByteSpan out)
        { return file->read(offset, out); };
        return response;
    };
}

// Embedded app resources + the `audiofile` scheme that streams the listed
// files off disk into the page's inline players. The app owns and registers
// the scheme itself. The roots bound which directories the page may read:
// only ~/Downloads and the extracted bundled assets, nothing else on disk.
WebView::Options dragOutOptions()
{
    auto options = embeddedOptions(category);
    options.streamingSchemes[audioScheme] =
        diskFileProvider({downloadsDir().string(), bundledAssetDir().string()});
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
        setApplicationMenuBar(buildDefaultWebViewMenuBar());
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
    eacp::Apps::run<MyApp>();
    return 0;
}
