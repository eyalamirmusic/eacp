#include "Common.h"

using namespace nano;

namespace FilePathNamesTests
{
using eacp::FilePath;
using eacp::Files::legalFileName;

auto tFileName = test("FilePathNames/fileName") = []
{
    check(FilePath {"dir/sub/image.png"}.fileName() == "image.png");
    check(FilePath {"image.png"}.fileName() == "image.png");
    check(FilePath {"/"}.fileName().empty());
    check(FilePath {"dir/"}.fileName().empty());
    check(FilePath {}.fileName().empty());
};

auto tStem = test("FilePathNames/stemRemovesOnlyTheLastExtension") = []
{
    check(FilePath {"dir/image.png"}.stem() == "image");
    check(FilePath {"dir/archive.tar.gz"}.stem() == "archive.tar");
    check(FilePath {"dir/noext"}.stem() == "noext");
    check(FilePath {"dir/.bashrc"}.stem() == ".bashrc");
    check(FilePath {"dir.d/noext"}.stem() == "noext");
    check(FilePath {"trailing."}.stem() == "trailing");
};

auto tHasExtension = test("FilePathNames/hasExtension") = []
{
    auto wav = FilePath {"dir/Kick.WAV"};

    check(wav.hasExtension("wav"));
    check(wav.hasExtension(".wav"));
    check(wav.hasExtension(".Wav"));
    check(!wav.hasExtension("av"));
    check(!wav.hasExtension("flac"));
    check(!wav.hasExtension(""));

    check(wav.hasExtension("flac;wav"));
    check(wav.hasExtension("flac; wav"));
    check(wav.hasExtension(".aif ; .wav ;"));
    check(!wav.hasExtension("flac;aif"));

    check(FilePath {"a.tar.gz"}.hasExtension("tar.gz"));
    check(FilePath {"dir.d/noext"}.hasExtension(""));
    check(!FilePath {"dir.d/noext"}.hasExtension("d"));

    // A dotfile has no extension, as extension() says.
    check(FilePath {"dir/.bashrc"}.extension().empty());
    check(FilePath {"dir/.bashrc"}.hasExtension(""));
    check(!FilePath {"dir/.config.json"}.hasExtension(""));
};

auto tWithExtension = test("FilePathNames/withExtension") = []
{
    check(FilePath {"dir/a.txt"}.withExtension("xml").str() == "dir/a.xml");
    check(FilePath {"dir/a.txt"}.withExtension(".xml").str() == "dir/a.xml");
    check(FilePath {"dir/a.tar.gz"}.withExtension("zip").str() == "dir/a.tar.zip");
    check(FilePath {"dir/a"}.withExtension("txt").str() == "dir/a.txt");
    check(FilePath {"dir/a.txt"}.withExtension("").str() == "dir/a");
    check(FilePath {"a.txt"}.withExtension("md").str() == "a.md");
    check(FilePath {}.withExtension("txt").empty());
};

auto tSibling = test("FilePathNames/sibling") = []
{
    check(FilePath {"dir/a.txt"}.sibling("b.txt").str() == "dir/b.txt");
    check(FilePath {"/a.txt"}.sibling("b.txt").str() == "/b.txt");
    check(FilePath {"a.txt"}.sibling("b.txt").str() == "b.txt");
};

auto tIsAbsolute = test("FilePathNames/isAbsolute") = []
{
    check(FilePath {"/usr/lib"}.isAbsolute());
    check(!FilePath {"usr/lib"}.isAbsolute());
    check(!FilePath {""}.isAbsolute());
    check(FilePath::tempDirectory().isAbsolute());

    if constexpr (eacp::Platform::isWindows())
    {
        check(FilePath {"C:/Windows"}.isAbsolute());
        check(FilePath {"\\\\server\\share"}.isAbsolute());
        check(FilePath {"//server/share"}.isAbsolute());
        check(FilePath {"C:\\Windows"}.isAbsolute());
        check(!FilePath {"C:foo"}.isAbsolute());
        check(!FilePath {"C:"}.isAbsolute());
    }
    else
    {
        check(!FilePath {"C:/Windows"}.isAbsolute());
    }
};

auto tLegalStrips = test("FilePathNames/legalFileNameStripsIllegalCharacters") = []
{
    check(legalFileName("My Preset") == "My Preset");
    check(legalFileName("a\"b#c@d,e;f:g<h>i*j^k|l?m\\n/o") == "abcdefghijklmno");
    check(legalFileName("Kick: 808 / Hard?.wav") == "Kick 808  Hard.wav");
    check(legalFileName("caf\xC3\xA9 (1)") == "caf\xC3\xA9 (1)");
    check(legalFileName("").empty());
};

auto tLegalTruncates = test("FilePathNames/legalFileNameTruncatesTo128") = []
{
    auto longName = std::string(200, 'x');

    check(legalFileName(longName) == std::string(128, 'x'));
    check(legalFileName(std::string(128, 'x')) == std::string(128, 'x'));

    // A short extension survives the cut.
    auto withExtension = legalFileName(longName + ".wav");
    check(withExtension.size() == 128);
    check(withExtension == std::string(124, 'x') + ".wav");

    // A dot further back than 11 characters is not treated as an extension.
    auto farDot = legalFileName("a." + longName);
    check(farDot == "a." + std::string(126, 'x'));

    // Counted in characters, not bytes: 200 two-byte characters keep 128.
    auto accented = std::string {};

    for (auto i = 0; i < 200; ++i)
        accented += "\xC3\xA9";

    auto cut = legalFileName(accented + ".txt");
    check(cut.size() == 124 * 2 + 4);
    check(cut.ends_with(".txt"));
};
} // namespace FilePathNamesTests
