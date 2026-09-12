#include "Common.h"

#include <eacp/Core/Utils/StdPath.h>
#include <eacp/Core/Utils/Zip.h>
#include <filesystem>
#include <fstream>

using namespace nano;
using eacp::FilePath;
using namespace eacp::Zip;

namespace
{
std::filesystem::path scratchDirectory(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / ("eacp-zip-" + name);

    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    return dir;
}

void write(const std::filesystem::path& path, std::string_view contents)
{
    std::filesystem::create_directories(path.parent_path());

    auto out = std::ofstream {path, std::ios::binary | std::ios::trunc};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string read(const std::filesystem::path& path)
{
    auto in = std::ifstream {path, std::ios::binary};

    return {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
}

std::string repeated(std::string_view unit, int times)
{
    auto text = std::string {};

    for (auto i = 0; i < times; ++i)
        text += unit;

    return text;
}

// The name is stored twice, in the local header and the central directory,
// and neither copy is under the CRC, so an entry can be renamed in place.
void renameEntry(Bytes& archive, std::string_view from, std::string_view to)
{
    check(from.size() == to.size());

    auto text = std::string {archive.begin(), archive.end()};

    for (auto at = text.find(from); at != std::string::npos;
         at = text.find(from, at))
        text.replace(at, from.size(), to);

    archive.assign(text.begin(), text.end());
}

Bytes twoEntryArchive()
{
    auto writer = Writer {};
    check(writer.add("hello.txt", "hello, zip"));
    check(writer.add("nested/data.bin", repeated("0123456789", 100)));

    return writer.finish();
}
} // namespace

auto tZipRoundTripsInMemory = test("Zip/roundTripsInMemory") = []
{
    const auto reader = Reader {twoEntryArchive()};

    check(reader.isValid());
    check(reader.numEntries() == 2);
    check(reader.contains("hello.txt"));
    check(reader.contains("nested/data.bin"));
    check(!reader.contains("missing.txt"));

    check(reader.readText("hello.txt") == "hello, zip");
    check(reader.readText("nested/data.bin") == repeated("0123456789", 100));
    check(!reader.read("missing.txt"));
};

auto tZipListsEntries = test("Zip/listsEntries") = []
{
    const auto reader = Reader {twoEntryArchive()};
    const auto entries = reader.entries();

    check(entries.size() == 2);
    check(entries[0].name == "hello.txt");
    check(entries[0].size == 10);
    check(!entries[0].isDirectory);
    check(entries[1].name == "nested/data.bin");
    check(entries[1].size == 1000);

    const auto found = reader.find("nested/data.bin");
    check(found.has_value());
    check(found->compressedSize < found->size);
};

auto tZipNamesAreCaseSensitive = test("Zip/namesAreCaseSensitive") = []
{
    const auto reader = Reader {twoEntryArchive()};

    check(reader.contains("hello.txt"));
    check(!reader.contains("HELLO.TXT"));
};

auto tZipStoresWithoutCompression = test("Zip/storesWithoutCompression") = []
{
    auto writer = Writer {};
    const auto payload = repeated("abcdefgh", 64);
    check(writer.add("stored.bin", payload, Level::none));

    const auto reader = Reader {writer.finish()};
    const auto entry = reader.find("stored.bin");

    check(entry.has_value());
    check(entry->compressedSize == entry->size);
    check(reader.readText("stored.bin") == payload);
};

auto tZipEmptyEntry = test("Zip/emptyEntryReadsBack") = []
{
    auto writer = Writer {};
    check(writer.add("empty.txt", std::string_view {}));

    const auto reader = Reader {writer.finish()};
    const auto entry = reader.find("empty.txt");

    check(entry.has_value());
    check(entry->size == 0);
    check(reader.readText("empty.txt") == "");
};

auto tZipRejectsGarbage = test("Zip/rejectsGarbage") = []
{
    const auto reader = Reader {Bytes {'n', 'o', 't', ' ', 'a', ' ', 'z', 'i', 'p'}};

    check(!reader.isValid());
    check(!reader.errorMessage().empty());
    check(reader.numEntries() == 0);
    check(reader.entries().empty());
    check(!reader.read("anything"));
};

auto tZipRejectsEmptyBytes =
    test("Zip/rejectsEmptyBytes") = [] { check(!Reader {Bytes {}}.isValid()); };

auto tZipMissingFileIsInvalid = test("Zip/missingFileIsInvalid") = []
{
    const auto dir = scratchDirectory("missing");
    const auto reader = Reader {FilePath {dir / "nope.zip"}};

    check(!reader.isValid());
};

auto tZipWritesAndReadsFile = test("Zip/writesAndReadsFile") = []
{
    const auto dir = scratchDirectory("file");
    const auto archive = FilePath {dir / "out" / "archive.zip"};

    auto writer = Writer {};
    check(writer.add("a.txt", "alpha"));
    check(writer.add("b.txt", "beta"));
    check(writer.writeTo(archive));

    const auto reader = Reader {archive};
    check(reader.isValid());
    check(reader.readText("a.txt") == "alpha");
    check(reader.readText("b.txt") == "beta");
};

auto tZipAddsFilesAndDirectories = test("Zip/addsFilesAndDirectories") = []
{
    const auto dir = scratchDirectory("tree");
    write(dir / "root.txt", "root");
    write(dir / "sub" / "one.txt", "one");
    write(dir / "sub" / "deeper" / "two.txt", "two");
    write(dir / "extra.txt", "extra");

    auto writer = Writer {};
    check(writer.addDirectory(FilePath {dir / "sub"}, "tree/"));
    check(writer.addFile("single.txt", FilePath {dir / "extra.txt"}));
    check(!writer.addFile("nope.txt", FilePath {dir / "nope.txt"}));
    check(!writer.errorMessage().empty());

    const auto reader = Reader {writer.finish()};
    check(reader.numEntries() == 3);
    check(reader.readText("tree/one.txt") == "one");
    check(reader.readText("tree/deeper/two.txt") == "two");
    check(reader.readText("single.txt") == "extra");
    check(!reader.contains("root.txt"));
};

auto tZipExtractsAll = test("Zip/extractsAll") = []
{
    const auto dir = scratchDirectory("extract");
    const auto reader = Reader {twoEntryArchive()};

    check(reader.extractAll(FilePath {dir / "out"}));
    check(read(dir / "out" / "hello.txt") == "hello, zip");
    check(read(dir / "out" / "nested" / "data.bin") == repeated("0123456789", 100));
};

auto tZipExtractionStaysUnderRoot = test("Zip/extractionStaysUnderRoot") = []
{
    const auto dir = scratchDirectory("slip");

    auto writer = Writer {};
    check(writer.add("../escaped.txt", "escaped"));
    check(writer.add("Xabsolute.txt", "absolute"));
    check(writer.add("ok/../fine.txt", "fine"));
    check(writer.add("safe.txt", "safe"));

    // The writer refuses a leading slash, so an archive carrying one has to
    // be forged the way a hostile tool would write it.
    auto bytes = writer.finish();
    renameEntry(bytes, "Xabsolute.txt", "/absolute.txt");

    const auto reader = Reader {std::move(bytes)};
    check(reader.contains("/absolute.txt"));

    const auto root = dir / "out";

    check(!reader.extractAll(FilePath {root}));
    check(read(root / "safe.txt") == "safe");
    check(read(root / "fine.txt") == "fine");
    check(!std::filesystem::exists(dir / "escaped.txt"));
    check(!std::filesystem::exists(root / "absolute.txt"));
};

auto tZipWriterRefusesAbsoluteNames = test("Zip/writerRefusesAbsoluteNames") = []
{
    auto writer = Writer {};

    check(!writer.add("/absolute.txt", "absolute"));
    check(!writer.errorMessage().empty());
    check(writer.add("relative.txt", "relative"));
};

auto tZipWriterEndsAfterFinish = test("Zip/writerEndsAfterFinish") = []
{
    auto writer = Writer {};
    check(writer.add("a.txt", "a"));
    check(!writer.finish().empty());

    check(!writer.add("b.txt", "b"));
    check(!writer.errorMessage().empty());
    check(writer.finish().empty());
};

auto tZipEmptyArchiveIsValid = test("Zip/emptyArchiveIsValid") = []
{
    auto writer = Writer {};
    const auto bytes = writer.finish();

    check(!bytes.empty());
    check(writer.errorMessage().empty());

    const auto reader = Reader {bytes};
    check(reader.isValid());
    check(reader.numEntries() == 0);
};

auto tZipCompressRoundTrips = test("Zip/compress/roundTrips") = []
{
    const auto text = repeated("the quick brown fox ", 200);
    const auto stream = compress(text);

    check(!stream.empty());
    check(stream.getSize() < text.size());
    check(decompressText(stream) == text);
};

auto tZipCompressEveryLevel = test("Zip/compress/everyLevel") = []
{
    const auto text = repeated("level ", 500);

    for (auto level: {Level::none, Level::fastest, Level::normal, Level::best})
    {
        const auto stream = compress(text, level);
        check(decompressText(stream) == text);
    }
};

auto tZipCompressEmpty = test("Zip/compress/empty") = []
{
    const auto stream = compress(std::string_view {});

    check(!stream.empty());
    check(decompressText(stream) == "");
};

auto tZipDecompressRejectsGarbage = test("Zip/decompress/rejectsGarbage") = []
{
    const auto garbage = Bytes {1, 2, 3, 4, 5, 6, 7, 8};

    const auto empty = Bytes {};

    check(!decompress(garbage));
    check(!decompress(empty));
};

auto tZipDecompressRejectsTruncated = test("Zip/decompress/rejectsTruncated") = []
{
    auto stream = compress(repeated("truncate me ", 100));
    stream.resize(stream.getSize() / 2);

    check(!decompress(stream));
};

auto tZipDecompressGrowsPastTheGuess = test("Zip/decompress/growsPastTheGuess") = []
{
    // Highly redundant input inflates to far more than four times the stream.
    const auto text = std::string(1 << 20, 'z');
    const auto stream = compress(text);

    check(stream.getSize() * 4 < text.size());
    check(decompressText(stream) == text);
};
