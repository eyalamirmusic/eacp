#pragma once

#include <string>
#include <string_view>

namespace eacp
{
template <typename T>
concept FilesystemPathLike = requires(const T& path) {
    typename T::value_type;
    { path.generic_u8string() };
};

// A filesystem path carried as UTF-8 text, so public headers never need
// <filesystem>. Deliberately minimal: it stores, joins and inspects text;
// everything that touches the filesystem converts at the boundary via
// StdPath.h.
class FilePath
{
public:
    FilePath() = default;
    FilePath(std::string textToUse);
    FilePath(std::string_view textToUse);
    FilePath(const char* textToUse);

    // Accepts std::filesystem::path without this header naming it: the
    // template only instantiates at call sites that already include
    // <filesystem>. generic_u8string() keeps non-ASCII intact on Windows.
    template <FilesystemPathLike P>
    FilePath(const P& path)
    {
        auto u8 = path.generic_u8string();
        text.assign(u8.begin(), u8.end());
    }

    // Well-known user directories, resolved through the native platform API
    // (NSSearchPathForDirectoriesInDomains, SHGetKnownFolderPath, the XDG
    // user dirs on Linux). Empty when the platform can't resolve them.
    static FilePath homeDirectory();
    static FilePath documentsDirectory();
    static FilePath downloadsDirectory();
    static FilePath musicDirectory();
    static FilePath moviesDirectory();
    static FilePath picturesDirectory();
    static FilePath desktopDirectory();
    static FilePath tempDirectory();

    // Per-user application data and cache roots: Application Support and
    // Caches on Apple platforms, Roaming and Local AppData on Windows, the
    // XDG data and cache homes on Linux.
    static FilePath appDataDirectory();
    static FilePath cacheDirectory();

    const std::string& str() const;
    const char* c_str() const;
    bool empty() const;

    // ".png" for "dir/image.png"; empty for dotfiles and extension-less
    // names, mirroring std::filesystem::path::extension().
    std::string extension() const;

    // "dir/sub" for "dir/sub/image.png"; empty when there is no directory
    // part, mirroring std::filesystem::path::parent_path().
    FilePath parentDirectory() const;

    FilePath operator/(std::string_view part) const;

    bool operator==(const FilePath& other) const = default;

private:
    std::string text;
};
} // namespace eacp
