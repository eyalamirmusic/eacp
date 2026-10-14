#include "Common.h"

using namespace nano;

namespace FilesWildcardTests
{
namespace fs = std::filesystem;
using eacp::FilePath;
using eacp::Files::matchesWildcard;

auto tStarAndQuestion = test("FilesWildcard/starAndQuestionMark") = []
{
    check(matchesWildcard("kick.wav", "*.wav"));
    check(matchesWildcard("kick.wav", "*"));
    check(matchesWildcard("kick.wav", "k*"));
    check(matchesWildcard("kick.wav", "*ck*"));
    check(matchesWildcard("kick.wav", "kick.wav"));
    check(matchesWildcard("kick.wav", "k?ck.w?v"));
    check(matchesWildcard("a.b.c", "*.c"));
    check(matchesWildcard("", "*"));

    check(!matchesWildcard("kick.wav", "*.flac"));
    check(!matchesWildcard("kick.wav", "kick"));
    check(!matchesWildcard("kick.wav", "?kick.wav"));
    check(!matchesWildcard("kick", "*.*"));
    check(!matchesWildcard("kick.wav", "kick.wa"));

    // '?' takes one character, not one byte.
    check(matchesWildcard("caf\xC3\xA9.txt", "caf?.txt"));

    // A stray continuation byte is still a character to match.
    check(!matchesWildcard("\x80"
                           "abc",
                           "abc"));
    check(matchesWildcard("\x80"
                          "abc",
                          "?abc"));
};

auto tCaseInsensitive = test("FilesWildcard/ignoresCase") = []
{
    check(matchesWildcard("KICK.WAV", "*.wav"));
    check(matchesWildcard("kick.wav", "*.WAV"));
    check(matchesWildcard("Snare.Flac", "s*.fLAC"));
};

auto tLists = test("FilesWildcard/separatedListsAreTrimmed") = []
{
    check(matchesWildcard("a.flac", "*.wav; *.flac"));
    check(matchesWildcard("a.wav", "*.wav; *.flac"));
    check(matchesWildcard("a.aif", " *.wav ;*.flac;  *.aif  "));
    check(matchesWildcard("a.aif", "*.wav,*.aif"));
    check(!matchesWildcard("a.mp3", "*.wav; *.flac"));

    check(matchesWildcard("anything", ""));
    check(matchesWildcard("anything", " ; "));
};

std::vector<std::string> listedNames(const fs::path& root,
                                     const eacp::Files::DirectoryOptions& options)
{
    auto names = std::vector<std::string> {};

    for (const auto& file: eacp::Files::listFiles(FilePath {root}, options))
        names.push_back(
            eacp::toStdPath(file).lexically_relative(root).generic_string());

    return names;
}

auto tListingFiltersFiles = test("FilesWildcard/listingFiltersFilesOnly") = []
{
    auto dir = scratchDirectory("wildcard-listing");
    writeTestFile(dir / "a.wav", "");
    writeTestFile(dir / "b.FLAC", "");
    writeTestFile(dir / "c.mp3", "");
    writeTestFile(dir / "notes.txt", "");
    writeTestFile(dir / "drums/kick.wav", "");
    writeTestFile(dir / "drums/kick.txt", "");
    writeTestFile(dir / "drums.wav/inside.flac", "");

    auto options = eacp::Files::DirectoryOptions {};
    options.recursive = true;
    options.pattern = "*.wav; *.flac";

    check(listedNames(dir, options)
          == std::vector<std::string> {
              "a.wav", "b.FLAC", "drums/kick.wav", "drums.wav/inside.flac"});

    // Directories are still visited whatever their name.
    auto entries = eacp::Files::listDirectory(FilePath {dir}, options);
    check(relativeNames(dir, entries)
          == std::vector<std::string> {"a.wav",
                                       "b.FLAC",
                                       "drums",
                                       "drums/kick.wav",
                                       "drums.wav",
                                       "drums.wav/inside.flac"});

    options.pattern.clear();
    check(listedNames(dir, options).size() == 7);

    fs::remove_all(dir);
};
} // namespace FilesWildcardTests
