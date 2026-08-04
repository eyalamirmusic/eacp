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
    // <filesystem>. Not generic_u8string(), which throws on names that are
    // not valid UTF-16 (NTFS permits unpaired surrogates): this conversion
    // never throws, mapping invalid units to U+FFFD, and keeps non-ASCII
    // intact on Windows.
    template <FilesystemPathLike P>
    FilePath(const P& path)
    {
        const auto& native = path.native();

        if constexpr (sizeof(typename P::value_type) == sizeof(char))
            text.assign(native.begin(), native.end());
        else
            assignFromWide({native.data(), native.size()});
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

    // Builds a path from a native wide string — a Win32 out-parameter, a
    // GetModuleFileNameW buffer. Converts to UTF-8 and normalizes '\' to '/',
    // the same treatment the std::filesystem::path constructor above gets.
    static FilePath fromWide(std::wstring_view wide);

    const std::string& str() const;
    const char* c_str() const;
    bool empty() const;

    // The path as a wide string, for native APIs and std::filesystem on
    // Windows. Never throws: bytes that are not valid UTF-8 decode to U+FFFD.
    std::wstring wide() const;

    // ".png" for "dir/image.png"; empty for dotfiles and extension-less
    // names, mirroring std::filesystem::path::extension().
    std::string extension() const;

    // "dir/sub" for "dir/sub/image.png"; empty when there is no directory
    // part, mirroring std::filesystem::path::parent_path().
    FilePath parentDirectory() const;

    FilePath operator/(std::string_view part) const;

    bool operator==(const FilePath& other) const = default;

private:
    // UTF-16 or UTF-32 (by wchar_t width) to UTF-8, with '\' normalized to
    // '/' and invalid units mapped to U+FFFD — never throws.
    void assignFromWide(std::wstring_view wide);

    std::string text;
};
} // namespace eacp
