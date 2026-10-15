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

// Incompressible bytes, the same for the same seed (xorshift32).
std::string noise(std::size_t size, std::uint32_t seed)
{
    auto text = std::string(size, '\0');
    auto state = seed == 0 ? 1u : seed;

    for (auto& c: text)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        c = static_cast<char>(state & 0xff);
    }

    return text;
}

// Big enough that every streamed entry in it takes several chunks.
const auto bigSize = 3 * extractChunkSize + 12345;

Bytes mixedArchive()
{
    auto writer = Writer {};
    check(writer.add("readme.txt", "streamed to disk"));
    check(writer.add("empty/", std::string_view {}));
    check(writer.add("deep/er/big.bin", noise(bigSize, 7)));
    check(writer.add("deep/stored.bin", noise(bigSize / 2, 11), Level::none));
    check(writer.add("deep/er/zeros.bin", std::string(bigSize, '\0')));
    check(writer.add("zero.txt", std::string_view {}));

    return writer.finish();
}

// Every report, checked as it arrives: totals fixed, counters never going
// down, and bytes moving at most one chunk per report.
struct ProgressLog
{
    ExtractProgressFunc func()
    {
        return [this](const ExtractProgress& progress)
        {
            if (!reports.empty())
            {
                const auto& last = reports.back();

                check(progress.entriesTotal == last.entriesTotal);
                check(progress.bytesTotal == last.bytesTotal);
                check(progress.entriesDone >= last.entriesDone);
                check(progress.bytesDone >= last.bytesDone);
            }

            check(progress.entriesDone <= progress.entriesTotal);
            check(progress.bytesDone <= progress.bytesTotal);

            reports.push_back(progress);

            return true;
        };
    }

    bool finished() const
    {
        const auto& last = reports.back();
        return last.entriesDone == last.entriesTotal
               && last.bytesDone == last.bytesTotal;
    }

    std::vector<ExtractProgress> reports;
};
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
    const auto dir = scratchDirectory("zip-missing");
    const auto reader = Reader {FilePath {dir / "nope.zip"}};

    check(!reader.isValid());
};

auto tZipWritesAndReadsFile = test("Zip/writesAndReadsFile") = []
{
    const auto dir = scratchDirectory("zip-file");
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
    const auto dir = scratchDirectory("zip-tree");
    writeTestFile(dir / "root.txt", "root");
    writeTestFile(dir / "sub" / "one.txt", "one");
    writeTestFile(dir / "sub" / "deeper" / "two.txt", "two");
    writeTestFile(dir / "extra.txt", "extra");

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
    const auto dir = scratchDirectory("zip-extract");
    const auto reader = Reader {twoEntryArchive()};

    check(reader.extractAll(FilePath {dir / "out"}));
    check(readTestFile(dir / "out" / "hello.txt") == "hello, zip");
    check(readTestFile(dir / "out" / "nested" / "data.bin")
          == repeated("0123456789", 100));
};

auto tZipExtractionStaysUnderRoot = test("Zip/extractionStaysUnderRoot") = []
{
    const auto dir = scratchDirectory("zip-slip");

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
    check(readTestFile(root / "safe.txt") == "safe");
    check(readTestFile(root / "fine.txt") == "fine");
    check(!std::filesystem::exists(dir / "escaped.txt"));
    check(!std::filesystem::exists(root / "absolute.txt"));
};

auto tZipStreamsEveryEntry = test("Zip/extract/streamsEveryEntry") = []
{
    const auto dir = scratchDirectory("zip-stream-all");
    const auto reader = Reader {mixedArchive()};
    const auto root = dir / "out";

    auto log = ProgressLog {};
    check(reader.extractAll(FilePath {root}, log.func()));

    for (const auto& entry: reader.entries())
    {
        if (entry.isDirectory)
            check(std::filesystem::is_directory(root / entry.name));
        else
            check(readTestFile(root / entry.name) == reader.readText(entry.name));
    }

    check(readTestFile(root / "deep" / "er" / "big.bin") == noise(bigSize, 7));
    check(std::filesystem::file_size(root / "zero.txt") == 0);
};

auto tZipExtractAllWithoutProgressStreams =
    test("Zip/extract/allWithoutProgress") = []
{
    const auto dir = scratchDirectory("zip-stream-plain");
    const auto reader = Reader {mixedArchive()};

    check(reader.extractAll(FilePath {dir}));
    check(readTestFile(dir / "deep" / "stored.bin") == noise(bigSize / 2, 11));
    check(std::filesystem::is_directory(dir / "empty"));
};

auto tZipExtractsOneEntry = test("Zip/extract/oneEntry") = []
{
    const auto dir = scratchDirectory("zip-stream-one");
    const auto reader = Reader {mixedArchive()};
    const auto target = dir / "a" / "b" / "copy.bin";

    check(reader.extract("deep/er/big.bin", FilePath {target}));
    check(readTestFile(target) == noise(bigSize, 7));

    // Replaces a longer file rather than leaving its tail.
    writeTestFile(dir / "small.txt", repeated("old contents ", 100));
    check(reader.extract("readme.txt", FilePath {dir / "small.txt"}));
    check(readTestFile(dir / "small.txt") == "streamed to disk");

    check(reader.extract("zero.txt", FilePath {dir / "zero.txt"}));
    check(std::filesystem::file_size(dir / "zero.txt") == 0);

    check(!reader.extract("missing.bin", FilePath {dir / "missing.bin"}));
    check(!std::filesystem::exists(dir / "missing.bin"));

    check(!reader.extract("DEEP/ER/BIG.BIN", FilePath {dir / "case.bin"}));
    check(!reader.extract("empty/", FilePath {dir / "dir"}));
    check(!std::filesystem::exists(dir / "dir"));

    check(!Reader {Bytes {}}.extract("readme.txt", FilePath {dir / "x"}));
};

auto tZipExtractOneEntryReportsProgress =
    test("Zip/extract/oneEntryReportsProgress") = []
{
    const auto dir = scratchDirectory("zip-stream-one-progress");
    const auto reader = Reader {mixedArchive()};

    auto log = ProgressLog {};
    check(reader.extract("deep/er/big.bin", FilePath {dir / "big.bin"}, log.func()));

    check(log.finished());
    check(log.reports.front().entriesTotal == 1);
    check(log.reports.front().bytesTotal == bigSize);
    check(log.reports.front().bytesDone == 0);

    // One before, one per chunk, one at the end.
    check(log.reports.size() == bigSize / extractChunkSize + 1 + 2);
};

auto tZipExtractReportsProgress = test("Zip/extract/reportsProgress") = []
{
    const auto dir = scratchDirectory("zip-stream-progress");
    const auto reader = Reader {mixedArchive()};

    auto log = ProgressLog {};
    check(reader.extractAll(FilePath {dir}, log.func()));

    auto total = std::uint64_t {0};

    for (const auto& entry: reader.entries())
        total += entry.size;

    const auto& first = log.reports.front();
    check(first.entriesDone == 0);
    check(first.bytesDone == 0);
    check(first.entriesTotal == reader.numEntries());
    check(first.bytesTotal == total);
    check(log.finished());

    // Bytes move a chunk at a time at most, so no report ever stood for more
    // than one chunk buffer of inflated data, and the big entries took many.
    auto chunkReports = 0;

    for (auto i = std::size_t {1}; i < log.reports.size(); ++i)
    {
        const auto step = log.reports[i].bytesDone - log.reports[i - 1].bytesDone;
        check(step <= extractChunkSize);

        if (step > 0)
            ++chunkReports;
    }

    check(chunkReports >= int((bigSize * 2 + bigSize / 2) / extractChunkSize));

    // One report after every entry as well.
    auto entryReports = 0;

    for (auto i = std::size_t {1}; i < log.reports.size(); ++i)
        if (log.reports[i].entriesDone != log.reports[i - 1].entriesDone)
            ++entryReports;

    check(entryReports == reader.numEntries());
};

auto tZipExtractCancelsMidEntry = test("Zip/extract/cancelsMidEntry") = []
{
    const auto dir = scratchDirectory("zip-stream-cancel");
    const auto reader = Reader {mixedArchive()};

    auto refused = false;
    auto calledAfterRefusing = false;
    auto lastProgress = ExtractProgress {};
    const auto cancelOnFirstChunk = [&](const ExtractProgress& progress)
    {
        calledAfterRefusing = calledAfterRefusing || refused;
        lastProgress = progress;
        refused = progress.bytesDone > std::string_view {"streamed to disk"}.size();
        return !refused;
    };

    check(!reader.extractAll(FilePath {dir}, cancelOnFirstChunk));

    // readme.txt and the directory came before the big entry; the big entry
    // was cut short and removed, and nothing after it was started.
    check(readTestFile(dir / "readme.txt") == "streamed to disk");
    check(std::filesystem::is_directory(dir / "empty"));
    check(!std::filesystem::exists(dir / "deep" / "er" / "big.bin"));
    check(!std::filesystem::exists(dir / "deep" / "stored.bin"));
    check(!std::filesystem::exists(dir / "zero.txt"));
    check(refused);
    check(!calledAfterRefusing);
    check(lastProgress.entriesDone == 2);
    check(lastProgress.bytesDone == 16 + extractChunkSize);

    // Cancelling one entry removes the file it had started.
    const auto target = dir / "single.bin";
    const auto stopAtOnce = [](const ExtractProgress& progress)
    { return progress.bytesDone == 0; };

    check(!reader.extract("deep/er/big.bin", FilePath {target}, stopAtOnce));
    check(!std::filesystem::exists(target));
};

auto tZipExtractCancelsBetweenEntries =
    test("Zip/extract/cancelsBetweenEntries") = []
{
    const auto dir = scratchDirectory("zip-stream-cancel-between");
    const auto reader = Reader {mixedArchive()};

    auto calls = 0;
    const auto stopAfterFirstEntry = [&](const ExtractProgress& progress)
    {
        ++calls;
        return progress.entriesDone < 1;
    };

    check(!reader.extractAll(FilePath {dir}, stopAfterFirstEntry));
    check(readTestFile(dir / "readme.txt") == "streamed to disk");
    check(!std::filesystem::exists(dir / "empty"));

    // Before, readme's one chunk, readme done - and nothing further.
    check(calls == 3);

    // Cancelling before anything starts writes nothing at all.
    const auto empty = dir / "none";
    check(!reader.extractAll(FilePath {empty},
                             [](const ExtractProgress&) { return false; }));
    check(!std::filesystem::exists(empty / "readme.txt"));
};

auto tZipExtractSkipsEscapingEntries = test("Zip/extract/skipsEscapingEntries") = []
{
    const auto dir = scratchDirectory("zip-stream-slip");

    auto writer = Writer {};
    check(writer.add("../evil.txt", "evil"));
    check(writer.add("a\\..\\..\\evil2.txt", "evil"));
    check(writer.add("safe.txt", "safe"));
    check(writer.add("win\\style.txt", "backslash"));

    const auto reader = Reader {writer.finish()};
    const auto root = dir / "out";

    auto log = ProgressLog {};
    check(!reader.extractAll(FilePath {root}, log.func()));

    check(readTestFile(root / "safe.txt") == "safe");
    check(readTestFile(root / "win" / "style.txt") == "backslash");
    check(!std::filesystem::exists(dir / "evil.txt"));
    check(!std::filesystem::exists(dir / "evil2.txt"));
    check(!std::filesystem::exists(root / "evil.txt"));
    check(log.finished());
};

auto tZipExtractKeepsGoingPastCorruptEntry =
    test("Zip/extract/keepsGoingPastCorruptEntry") = []
{
    const auto dir = scratchDirectory("zip-stream-corrupt");
    const auto payload = noise(2 * extractChunkSize + 99, 3);

    auto writer = Writer {};
    check(writer.add("before.txt", "before"));
    check(writer.add("bad.bin", payload, Level::none));
    check(writer.add("after.txt", "after"));
    auto bytes = writer.finish();

    // Stored, so the payload sits in the archive verbatim; flip a byte deep
    // inside it and only the CRC can tell.
    auto text = std::string {bytes.begin(), bytes.end()};
    const auto at = text.find(payload.substr(0, 64));
    check(at != std::string::npos);
    bytes[static_cast<int>(at + extractChunkSize + 5)] ^= 0xff;

    const auto reader = Reader {std::move(bytes)};

    auto log = ProgressLog {};
    check(!reader.extractAll(FilePath {dir}, log.func()));

    check(readTestFile(dir / "before.txt") == "before");
    check(readTestFile(dir / "after.txt") == "after");
    check(!std::filesystem::exists(dir / "bad.bin"));
    check(log.finished());

    check(!reader.extract("bad.bin", FilePath {dir / "bad.bin"}));
    check(!std::filesystem::exists(dir / "bad.bin"));
};

auto tZipExtractsUtf8Names = test("Zip/extract/utf8Names") = []
{
    const auto dir = scratchDirectory("zip-stream-utf8");
    const auto name =
        std::string {"caf\xc3\xa9/\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d.txt"};

    auto writer = Writer {};
    check(writer.add(name, "unicode"));

    const auto reader = Reader {writer.finish()};
    check(reader.extractAll(FilePath {dir}));
    check(eacp::Files::readFile(FilePath {FilePath {dir}.str() + "/" + name})
          == "unicode");
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
