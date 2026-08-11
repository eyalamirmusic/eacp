#pragma once

#include <string>
#include <string_view>

namespace eacp
{
template <typename T>
concept FilesystemPathLike = requires(const T& path) {
    typename T::value_type;
    { path.native() };
    { path.generic_u8string() };
};

// A filesystem path carried as UTF-8 text, so public headers never need
// <filesystem>. It only stores, joins and inspects text; anything that touches
// the filesystem converts at the boundary via StdPath.h.
class FilePath
{
public:
    FilePath() = default;
    FilePath(std::string textToUse);
    FilePath(std::string_view textToUse);
    FilePath(const char* textToUse);

    // Accepts std::filesystem::path without this header naming it. Not via
    // generic_u8string(), which throws on the unpaired surrogates NTFS permits;
    // this never throws, mapping invalid units to U+FFFD.
    template <FilesystemPathLike P>
    FilePath(const P& path)
    {
        const auto& native = path.native();

        if constexpr (sizeof(typename P::value_type) == sizeof(char))
            text.assign(native.begin(), native.end());
        else
            assignFromWide({native.data(), native.size()});
    }

    // Resolved through the native platform API. Empty when it can't resolve them.
    static FilePath homeDirectory();
    static FilePath documentsDirectory();
    static FilePath downloadsDirectory();
    static FilePath musicDirectory();
    static FilePath moviesDirectory();
    static FilePath picturesDirectory();
    static FilePath desktopDirectory();
    static FilePath tempDirectory();

    // Per-user roots: Application Support and Caches on Apple, Roaming and Local
    // AppData on Windows, the XDG data and cache homes on Linux.
    static FilePath appDataDirectory();
    static FilePath cacheDirectory();

    const std::string& str() const;
    const char* c_str() const;
    bool empty() const;

    // Never throws: bytes that are not valid UTF-8 decode to U+FFFD.
    std::wstring wide() const;

    // ".png" for "dir/image.png"; empty for dotfiles and extension-less names.
    std::string extension() const;

    // "dir/sub" for "dir/sub/image.png"; empty when there is no directory part.
    FilePath parentDirectory() const;

    FilePath operator/(std::string_view part) const;

    bool operator==(const FilePath& other) const = default;

private:
    // Normalizes '\' to '/' and maps invalid units to U+FFFD; never throws.
    void assignFromWide(std::wstring_view wide);

    std::string text;
};
} // namespace eacp
