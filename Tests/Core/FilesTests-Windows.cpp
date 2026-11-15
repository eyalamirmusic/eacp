// What writeFileAtomically has to do on Windows to keep the promise rename(2)
// makes on POSIX: replace a destination that another handle holds open, or
// one that is read-only, without the destination ever going missing. The
// portable half is in FilesTests.cpp, the POSIX one in FilesTests-Posix.cpp.

#include "Common.h"
#include <eacp/Core/Utils/FilesPlatform.h>
#include <eacp/Core/Utils/StdPath.h>
#include <eacp/Core/Utils/WinInclude.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace nano;
using eacp::FilePath;
using eacp::Detail::ReplaceStep;

// Named, so its helpers cannot meet FilesTests.cpp's in a unity batch.
namespace FilesWindowsTests
{
namespace
{
// A handle on path held for the scope, as an indexer, a virus scanner or
// another program with the file loaded would hold one.
struct OpenHandle
{
    OpenHandle(const std::filesystem::path& path, DWORD share)
        : handle(CreateFileW(path.c_str(),
                             GENERIC_READ,
                             share,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr))
    {
    }

    ~OpenHandle()
    {
        if (isOpen())
            CloseHandle(handle);
    }

    OpenHandle(const OpenHandle&) = delete;
    OpenHandle& operator=(const OpenHandle&) = delete;

    bool isOpen() const { return handle != INVALID_HANDLE_VALUE; }

    // Everything the handle reads from the start, whichever file it is on now.
    std::string readAll() const
    {
        auto start = LARGE_INTEGER {};
        SetFilePointerEx(handle, start, nullptr, FILE_BEGIN);

        auto text = std::string {};
        char buffer[256];
        auto got = DWORD {0};

        while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            text.append(buffer, got);

        return text;
    }

    HANDLE handle;
};

constexpr auto shareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr auto shareAllButDelete = FILE_SHARE_READ | FILE_SHARE_WRITE;

int entryCount(const std::filesystem::path& dir)
{
    auto count = 0;

    for ([[maybe_unused]] auto& entry: std::filesystem::directory_iterator(dir))
        ++count;

    return count;
}

bool isReadOnly(const std::filesystem::path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());

    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
}

void setReadOnly(const std::filesystem::path& path, bool readOnly)
{
    auto attributes = GetFileAttributesW(path.c_str());

    if (readOnly)
        attributes |= FILE_ATTRIBUTE_READONLY;
    else
        attributes &= ~DWORD {FILE_ATTRIBUTE_READONLY};

    SetFileAttributesW(path.c_str(), attributes);
}

bool atomicWriteThrows(const std::filesystem::path& path, std::string_view text)
{
    try
    {
        writeAtomically(path, text);
    }
    catch (const std::runtime_error&)
    {
        return true;
    }

    return false;
}
} // namespace

auto tAtomicReplacesFileHeldOpen = test("Files/atomicReplacesFileHeldOpen") = []
{
    auto dir = scratchDirectory("files-held-open");
    auto path = dir / "doc.txt";

    writeTestFile(path, "old");

    {
        auto holder = OpenHandle {path, shareAll};
        check(holder.isOpen());

        check(!atomicWriteThrows(path, "new"));

        // The name now leads to the new file, while the handle opened before
        // the save keeps the old one, exactly as an open descriptor does
        // across rename(2).
        check(readTestFile(path) == "new");
        check(holder.readAll() == "old");
        check(OpenHandle(path, shareAll).readAll() == "new");
    }

    check(entryCount(dir) == 1);
    check(readTestFile(path) == "new");

    std::filesystem::remove_all(dir);
};

// A handle opened without FILE_SHARE_DELETE forbids every rename of the file
// and every rename onto it, POSIX semantics included, until it is closed:
// nothing can replace it. The save fails, and fails cleanly, read-only or not:
// the file whole, its attribute as it was, and the temporary (read-only too in
// the second round, carrying the attribute over) removed.
auto tAtomicFailsCleanlyOnFileHeldWithoutShareDelete =
    test("Files/atomicFailsCleanlyOnFileHeldWithoutShareDelete") = []
{
    for (auto readOnly: {false, true})
    {
        auto dir = scratchDirectory("files-held-exclusive");
        auto path = dir / "doc.txt";

        writeTestFile(path, "old");
        setReadOnly(path, readOnly);

        {
            auto holder = OpenHandle {path, shareAllButDelete};
            check(holder.isOpen());

            check(atomicWriteThrows(path, "new"));

            check(readTestFile(path) == "old");
            check(holder.readAll() == "old");
            check(isReadOnly(path) == readOnly);
            check(entryCount(dir) == 1);
        }

        // Once the handle is gone the same save goes through.
        check(!atomicWriteThrows(path, "new"));
        check(readTestFile(path) == "new");
        check(isReadOnly(path) == readOnly);
        check(entryCount(dir) == 1);

        setReadOnly(path, false);
        std::filesystem::remove_all(dir);
    }
};

// rename(2) replaces a read-only file in a writable directory, since only the
// directory's permissions decide, and the permission bits carried over make
// the replacement read-only too. Windows matches that: the save succeeds and
// the file stays read-only.
auto tAtomicReplacesReadOnlyFile = test("Files/atomicReplacesReadOnlyFile") = []
{
    auto dir = scratchDirectory("files-read-only");
    auto path = dir / "doc.txt";

    writeTestFile(path, "old");
    setReadOnly(path, true);

    check(!atomicWriteThrows(path, "new"));

    check(readTestFile(path) == "new");
    check(isReadOnly(path));
    check(entryCount(dir) == 1);

    setReadOnly(path, false);
    std::filesystem::remove_all(dir);
};

auto tAtomicReplacesReadOnlyFileHeldOpen =
    test("Files/atomicReplacesReadOnlyFileHeldOpen") = []
{
    auto dir = scratchDirectory("files-read-only-held");
    auto path = dir / "doc.txt";

    writeTestFile(path, "old");
    setReadOnly(path, true);

    {
        auto holder = OpenHandle {path, shareAll};
        check(holder.isOpen());

        check(!atomicWriteThrows(path, "new"));

        check(readTestFile(path) == "new");
        check(holder.readAll() == "old");
        check(isReadOnly(path));
    }

    check(entryCount(dir) == 1);

    setReadOnly(path, false);
    std::filesystem::remove_all(dir);
};

// A MemoryMappedFile is a share-everything handle and a live view: the save
// replaces the file under it, and the view keeps the old bytes, as an mmap
// keeps the old inode's.
auto tAtomicReplacesMappedFile = test("Files/atomicReplacesMappedFile") = []
{
    auto dir = scratchDirectory("files-mapped");
    auto path = dir / "doc.txt";

    writeTestFile(path, "old");

    {
        auto mapped = eacp::MemoryMappedFile {FilePath {path}};
        check(mapped.isValid());

        check(!atomicWriteThrows(path, "new"));

        check(mapped.text() == "old");
        check(readTestFile(path) == "new");
    }

    check(entryCount(dir) == 1);

    std::filesystem::remove_all(dir);
};

// The UTF-8 FilePath has to reach every wide call intact: the parent's
// creation, the temporary's, the rename and the cleanup. Escapes rather than
// literal characters, so the test reads the same under any source code page.
auto tAtomicReplacesNonAsciiPath = test("Files/atomicReplacesNonAsciiPath") = []
{
    auto root = scratchDirectory("files-non-ascii");
    auto dir = root / L"\u00FCn\u00EFc\u00F8d\u00E9";
    auto wideName = std::wstring {L"\u00D6lfarbe-\u65E5\u672C\u8A9E-\u266B.txt"};
    auto path = dir / wideName;

    // The same file as a UTF-8 FilePath, the form callers hand in.
    const auto utf8 = FilePath {path}.str();
    check(utf8.find("\xC3\x96lfarbe") != std::string::npos);
    check(FilePath {utf8}.wide() == path.wstring());

    eacp::Files::writeFileAtomically(FilePath {utf8}, std::string_view {"old"});
    check(readTestFile(path) == "old");

    {
        auto holder = OpenHandle {path, shareAll};
        check(holder.isOpen());

        check(!atomicWriteThrows(path, "new"));
        check(holder.readAll() == "old");
    }

    check(readTestFile(path) == "new");
    check(entryCount(dir) == 1);
    check(std::filesystem::directory_iterator(dir)->path().filename() == wideName);

    std::filesystem::remove_all(root);
};

// The fallback, driven directly: the system here has POSIX semantics, so
// replaceFile itself never reaches it.
auto tReplaceByMovingReplacesAFile = test("Files/replaceByMovingReplacesAFile") = []
{
    auto dir = scratchDirectory("files-move-replace");
    auto from = dir / "doc.txt.tmp";
    auto to = dir / "doc.txt";

    writeTestFile(from, "new");
    writeTestFile(to, "old");

    check(!eacp::Detail::replaceFileByMoving(from, to));

    check(readTestFile(to) == "new");
    check(entryCount(dir) == 1);

    std::filesystem::remove_all(dir);
};

auto tReplaceByMovingReplacesAReadOnlyFile =
    test("Files/replaceByMovingReplacesAReadOnlyFile") = []
{
    auto dir = scratchDirectory("files-move-read-only");
    auto from = dir / "doc.txt.tmp";
    auto to = dir / "doc.txt";

    // As writeFileAtomically leaves them: the attribute already carried over.
    writeTestFile(from, "new");
    writeTestFile(to, "old");
    setReadOnly(from, true);
    setReadOnly(to, true);

    check(!eacp::Detail::replaceFileByMoving(from, to));

    check(readTestFile(to) == "new");
    check(isReadOnly(to));
    check(entryCount(dir) == 1);

    setReadOnly(to, false);
    std::filesystem::remove_all(dir);
};

// MoveFileExW has no POSIX semantics, so where the fallback is all there is a
// file held open cannot be replaced at all; it fails without touching it.
auto tReplaceByMovingLeavesAFileHeldOpenWhole =
    test("Files/replaceByMovingLeavesAFileHeldOpenWhole") = []
{
    auto dir = scratchDirectory("files-move-held");
    auto from = dir / "doc.txt.tmp";
    auto to = dir / "doc.txt";

    writeTestFile(from, "new");
    writeTestFile(to, "old");
    setReadOnly(to, true);

    {
        auto holder = OpenHandle {to, shareAll};
        check(holder.isOpen());

        check(!!eacp::Detail::replaceFileByMoving(from, to));
    }

    // The read-only attribute it lifted to try is back.
    check(readTestFile(to) == "old");
    check(isReadOnly(to));
    check(readTestFile(from) == "new");

    setReadOnly(to, false);
    std::filesystem::remove_all(dir);
};

auto tPosixRenameRefusesAHolderWithoutShareDelete =
    test("Files/posixRenameRefusesAHolderWithoutShareDelete") = []
{
    auto dir = scratchDirectory("files-posix-exclusive");
    auto from = dir / "doc.txt.tmp";
    auto to = dir / "doc.txt";

    writeTestFile(from, "new");
    writeTestFile(to, "old");

    {
        auto holder = OpenHandle {to, shareAllButDelete};
        check(holder.isOpen());

        const auto error = eacp::Detail::replaceFileWithPosixSemantics(from, to);

        // nextReplaceStep stops on it: MoveFileExW would fail the same way.
        check(error.value() == static_cast<int>(ERROR_SHARING_VIOLATION));
    }

    check(readTestFile(to) == "old");
    check(readTestFile(from) == "new");

    std::filesystem::remove_all(dir);
};

// The whole fallback decision, every answer the steps can give. Only older
// systems, FAT, exFAT and network shares ever take most of these rows.
auto tNextReplaceStepTable = test("Files/nextReplaceStepTable") = []
{
    struct Row
    {
        ReplaceStep tried;
        unsigned long error;
        ReplaceStep next;
    };

    constexpr auto first = ReplaceStep::renameIgnoringReadOnly;
    constexpr auto retry = ReplaceStep::renameHonouringReadOnly;
    constexpr auto move = ReplaceStep::move;
    constexpr auto stop = ReplaceStep::stop;

    const Row rows[] = {
        {first, ERROR_SUCCESS, stop},
        {first, ERROR_SHARING_VIOLATION, stop},
        {first, ERROR_INVALID_PARAMETER, retry},
        {first, ERROR_INVALID_FUNCTION, move},
        {first, ERROR_NOT_SUPPORTED, move},
        {first, ERROR_ACCESS_DENIED, move},
        {first, ERROR_FILE_NOT_FOUND, stop},
        {first, ERROR_PATH_NOT_FOUND, stop},
        {first, ERROR_ALREADY_EXISTS, stop},
        {retry, ERROR_SUCCESS, stop},
        {retry, ERROR_SHARING_VIOLATION, stop},
        {retry, ERROR_INVALID_PARAMETER, move},
        {retry, ERROR_INVALID_FUNCTION, move},
        {retry, ERROR_NOT_SUPPORTED, move},
        {retry, ERROR_ACCESS_DENIED, move},
        {retry, ERROR_FILE_NOT_FOUND, stop},
        {move, ERROR_SUCCESS, stop},
        {move, ERROR_ACCESS_DENIED, stop},
        {move, ERROR_INVALID_PARAMETER, stop},
        {move, ERROR_SHARING_VIOLATION, stop},
    };

    for (const auto& row: rows)
        check(eacp::Detail::nextReplaceStep(row.tried, row.error) == row.next);
};

namespace
{
std::error_code windowsError(unsigned long code)
{
    return {static_cast<int>(code), std::system_category()};
}

struct ScriptedReplace
{
    std::vector<ReplaceStep> steps;
    std::error_code result;
};

// replaceFileUsing over attempts that answer the given codes in turn.
ScriptedReplace replaceAnswering(std::vector<unsigned long> answers)
{
    auto scripted = ScriptedReplace {};
    auto next = std::size_t {0};

    scripted.result = eacp::Detail::replaceFileUsing(
        [&](ReplaceStep step)
        {
            scripted.steps.push_back(step);

            if (next == answers.size())
                return windowsError(ERROR_GEN_FAILURE);

            const auto code = answers[next++];
            return code == ERROR_SUCCESS ? std::error_code {} : windowsError(code);
        });

    return scripted;
}
} // namespace

auto tReplaceFileUsingTakesTheSteps =
    test("Files/replaceFileUsingTakesTheSteps") = []
{
    constexpr auto first = ReplaceStep::renameIgnoringReadOnly;
    constexpr auto retry = ReplaceStep::renameHonouringReadOnly;
    constexpr auto move = ReplaceStep::move;

    struct Script
    {
        std::vector<unsigned long> answers;
        std::vector<ReplaceStep> steps;
        unsigned long result;
    };

    const Script scripts[] = {
        // Windows 10 1809 and later on NTFS.
        {{ERROR_SUCCESS}, {first}, ERROR_SUCCESS},
        {{ERROR_SHARING_VIOLATION}, {first}, ERROR_SHARING_VIOLATION},
        // 1709 to 1803: no read-only flag; a read-only target then moves.
        {{ERROR_INVALID_PARAMETER, ERROR_SUCCESS}, {first, retry}, ERROR_SUCCESS},
        {{ERROR_INVALID_PARAMETER, ERROR_ACCESS_DENIED, ERROR_SUCCESS},
         {first, retry, move},
         ERROR_SUCCESS},
        {{ERROR_INVALID_PARAMETER, ERROR_SHARING_VIOLATION},
         {first, retry},
         ERROR_SHARING_VIOLATION},
        // Before 1709, or a filesystem without the POSIX rename.
        {{ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER, ERROR_ACCESS_DENIED},
         {first, retry, move},
         ERROR_ACCESS_DENIED},
        {{ERROR_NOT_SUPPORTED, ERROR_SUCCESS}, {first, move}, ERROR_SUCCESS},
        {{ERROR_INVALID_FUNCTION, ERROR_SUCCESS}, {first, move}, ERROR_SUCCESS},
        // A filesystem that ignores the read-only flag.
        {{ERROR_ACCESS_DENIED, ERROR_SUCCESS}, {first, move}, ERROR_SUCCESS},
        {{ERROR_FILE_NOT_FOUND}, {first}, ERROR_FILE_NOT_FOUND},
    };

    for (const auto& script: scripts)
    {
        const auto scripted = replaceAnswering(script.answers);

        check(scripted.steps == script.steps);
        check(scripted.result.value() == static_cast<int>(script.result));
    }
};

// What Windows 10 1709 to 1803 do with a read-only target, on the real
// filesystem: the first step refused as those systems refuse the flag, the
// rename without it refused by the attribute, and the move lands the file,
// still read-only.
auto tReplaceWithoutTheReadOnlyFlagStillLands =
    test("Files/replaceWithoutTheReadOnlyFlagStillLands") = []
{
    auto dir = scratchDirectory("files-old-windows");
    auto from = dir / "doc.txt.tmp";
    auto to = dir / "doc.txt";

    writeTestFile(from, "new");
    writeTestFile(to, "old");
    setReadOnly(from, true);
    setReadOnly(to, true);

    auto steps = std::vector<ReplaceStep> {};
    auto answers = std::vector<int> {};

    const auto error = eacp::Detail::replaceFileUsing(
        [&](ReplaceStep step)
        {
            steps.push_back(step);

            auto answer = windowsError(ERROR_INVALID_PARAMETER);

            if (step == ReplaceStep::renameHonouringReadOnly)
                answer =
                    eacp::Detail::replaceFileWithPosixSemantics(from, to, false);
            else if (step == ReplaceStep::move)
                answer = eacp::Detail::replaceFileByMoving(from, to);

            answers.push_back(answer.value());
            return answer;
        });

    check(!error);
    check(steps
          == std::vector<ReplaceStep> {ReplaceStep::renameIgnoringReadOnly,
                                       ReplaceStep::renameHonouringReadOnly,
                                       ReplaceStep::move});
    check(answers.size() == 3
          && answers[1] == static_cast<int>(ERROR_ACCESS_DENIED));
    check(readTestFile(to) == "new");
    check(isReadOnly(to));
    check(entryCount(dir) == 1);

    setReadOnly(to, false);
    std::filesystem::remove_all(dir);
};
} // namespace FilesWindowsTests
