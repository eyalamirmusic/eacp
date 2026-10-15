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

    // This app's own folder under those roots: <root>/<Company>/<App>, e.g.
    // ~/Library/Application Support/Acme/My App. The names come from the
    // embedded AppInfo (Platform::getAppName / getCompanyName); the company
    // level is left out when there is none, and an app with no AppInfo is
    // named after its executable. The explicit overloads take the names
    // instead, for a plugin that shares its host's folder or a test.
    static FilePath appSupportDirectory();
    static FilePath appCacheDirectory();
    static FilePath appSupportDirectory(std::string_view company,
                                        std::string_view app);
    static FilePath appCacheDirectory(std::string_view company,
                                      std::string_view app);

    // Machine-wide application data, shared by every user: /Library on Apple
    // platforms, ProgramData on Windows, /opt on Linux (JUCE's choice). Empty
    // on Android.
    static FilePath commonAppDataDirectory();

    // Where applications are installed: /Applications on Apple platforms,
    // Program Files on Windows, /usr on Linux. Empty on Android.
    static FilePath applicationsDirectory();

    // The shared container of an Apple app group, for sandboxed apps and
    // plugins that share data. Empty when the group is unknown, the id is
    // empty, or the platform has no app groups.
    static FilePath appGroupContainerDirectory(std::string_view groupID);

    // Builds a path from a native wide string — a Win32 out-parameter, a
    // GetModuleFileNameW buffer. Converts to UTF-8 and normalizes '\' to '/',
    // the same treatment the std::filesystem::path constructor above gets.
    static FilePath fromWide(std::wstring_view wide);

    // Builds a path from UTF-8 text in the platform's own form — a string
    // nativeString() wrote out, one a user typed, one read from a settings
    // file. On Windows '\' becomes '/', so "C:\dir\file.wav" equals the
    // FilePath of "C:/dir/file.wav"; elsewhere '\' is an ordinary file name
    // character and the text is kept as-is, like the string constructors.
    static FilePath fromNative(std::string_view nativeText);

    // The text in eacp's own form: UTF-8 with '/' separators on every
    // platform, Windows included.
    const std::string& str() const;
    const char* c_str() const;
    bool empty() const;

    // The text the platform's own APIs and users expect, for display or for
    // storing where other software reads it: on Windows every '/' becomes '\',
    // so "C:/dir/file.wav" reads "C:\dir\file.wav", "//server/share" reads
    // "\\server\share" and a "//?/" long-path prefix reads "\\?\" again — the
    // inverse of what the wide constructors and fromWide do. Elsewhere it is
    // str(). fromNative() parses it back.
    std::string nativeString() const;

    // The path as a wide string, for native APIs and std::filesystem on
    // Windows. Never throws: bytes that are not valid UTF-8 decode to U+FFFD.
    std::wstring wide() const;

    // ".png" for "dir/image.png"; empty for dotfiles and extension-less
    // names, mirroring std::filesystem::path::extension().
    std::string extension() const;

    // "dir/sub" for "dir/sub/image.png"; empty when there is no directory
    // part, mirroring std::filesystem::path::parent_path().
    FilePath parentDirectory() const;

    // "image.png" for "dir/image.png": everything after the last separator.
    std::string fileName() const;

    // The file name with only its last extension removed: "a.tar" for
    // "dir/a.tar.gz". A dotfile keeps its whole name.
    std::string stem() const;

    // Case-insensitive, with or without the leading dot, or several
    // alternatives separated by ';' ("wav; flac"). An empty argument asks
    // whether there is no extension at all.
    bool hasExtension(std::string_view ext) const;

    // The same path with the last extension replaced by ext, given with or
    // without its dot. An empty ext strips the extension.
    FilePath withExtension(std::string_view ext) const;

    // A path beside this one: parentDirectory() / name.
    FilePath sibling(std::string_view name) const;

    // Rooted at '/', or on Windows at a drive letter ("C:") or a UNC share.
    bool isAbsolute() const;

    FilePath operator/(std::string_view part) const;

    bool operator==(const FilePath& other) const = default;

    //Reads the contents of the file to a std::string
    std::string read() const;

private:
    // UTF-16 or UTF-32 (by wchar_t width) to UTF-8, with '\' normalized to
    // '/' and invalid units mapped to U+FFFD — never throws.
    void assignFromWide(std::wstring_view wide);

    std::string text;
};
} // namespace eacp
