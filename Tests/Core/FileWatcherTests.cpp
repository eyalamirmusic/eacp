#include "Common.h"

#include <optional>

using namespace nano;

namespace FileWatcherTests
{
using eacp::FilePath;
using eacp::TemporaryFile;
using eacp::Vector;
using eacp::Files::FileWatcher;
using eacp::Files::FileWatcherOptions;
using eacp::Threads::runEventLoopUntil;
using eacp::Time::MS;

struct ChangeLog
{
    Vector<Vector<FilePath>> calls;

    FileWatcher::ChangedFunc recorder()
    {
        auto record = [this](const Vector<FilePath>& changed)
        { calls.add(changed); };
        return record;
    }

    bool waitForCalls(int count)
    {
        auto reached = [this, count] { return calls.size() >= count; };
        return runEventLoopUntil(reached, MS {5000});
    }
};

FileWatcherOptions fastOptions()
{
    auto options = FileWatcherOptions {};
    options.pollInterval = MS {20};
    options.settle = MS {50};
    return options;
}

void pumpFor(MS duration)
{
    auto never = [] { return false; };
    runEventLoopUntil(never, duration);
}

void writeFile(const FilePath& path, std::string_view contents)
{
    writeTestFile(eacp::toStdPath(path), contents);
}

bool contains(const Vector<FilePath>& paths, const FilePath& path)
{
    for (const auto& candidate: paths)
        if (candidate == path)
            return true;

    return false;
}

auto tModifiedFileFiresOnce =
    test("FileWatcher/modifiedFileFiresOnceAfterSettling") = []
{
    auto dir = TemporaryFile {};
    auto file = dir.path() / "source.cpp";
    writeFile(file, "a");

    auto log = ChangeLog {};
    auto watcher = FileWatcher {{dir.path()}, log.recorder(), fastOptions()};

    writeFile(file, "abc");

    check(log.waitForCalls(1));
    pumpFor(MS {300});

    check(log.calls.size() == 1);
    check(log.calls[0].size() == 1);
    check(log.calls[0][0] == file);
};

auto tWritesCoalesce = test("FileWatcher/writesInsideTheSettleWindowCoalesce") = []
{
    auto dir = TemporaryFile {};
    auto first = dir.path() / "first.cpp";
    auto second = dir.path() / "second.cpp";
    writeFile(first, "a");
    writeFile(second, "a");

    auto options = fastOptions();
    options.settle = MS {300};

    auto log = ChangeLog {};
    auto watcher = FileWatcher {{dir.path()}, log.recorder(), options};

    writeFile(first, "abc");
    pumpFor(MS {60});
    writeFile(second, "abcdef");

    check(log.waitForCalls(1));
    pumpFor(MS {500});

    check(log.calls.size() == 1);
    check(log.calls[0].size() == 2);
    check(contains(log.calls[0], first));
    check(contains(log.calls[0], second));
};

auto tPatternFilters = test("FileWatcher/filesNotMatchingThePatternAreIgnored") = []
{
    auto dir = TemporaryFile {};
    auto source = dir.path() / "main.cpp";
    auto other = dir.path() / "notes.txt";

    auto options = fastOptions();
    options.pattern = "*.cpp;*.h";

    auto log = ChangeLog {};
    auto watcher = FileWatcher {{dir.path()}, log.recorder(), options};

    writeFile(other, "ignored");
    pumpFor(MS {300});
    check(log.calls.empty());

    writeFile(source, "int main() {}");

    check(log.waitForCalls(1));
    check(log.calls[0].size() == 1);
    check(log.calls[0][0] == source);
};

auto tCreatedAndRemoved = test("FileWatcher/createdAndRemovedFilesAreReported") = []
{
    auto dir = TemporaryFile {};
    auto doomed = dir.path() / "nested/doomed.h";
    auto created = dir.path() / "nested/created.h";
    writeFile(doomed, "x");

    auto log = ChangeLog {};
    auto watcher = FileWatcher {{dir.path()}, log.recorder(), fastOptions()};

    eacp::Files::removeFile(doomed);
    writeFile(created, "yy");

    check(log.waitForCalls(1));
    pumpFor(MS {200});

    check(log.calls.size() == 1);
    check(log.calls[0].size() == 2);
    check(contains(log.calls[0], doomed));
    check(contains(log.calls[0], created));
};

auto tSingleFile = test("FileWatcher/watchesASingleFilePath") = []
{
    auto dir = TemporaryFile {};
    auto watched = dir.path() / "watched.txt";
    auto sibling = dir.path() / "sibling.txt";

    auto log = ChangeLog {};
    auto watcher = FileWatcher {{watched}, log.recorder(), fastOptions()};

    check(watcher.getPaths().size() == 1);
    check(watcher.getPaths()[0] == watched);

    writeFile(sibling, "not watched");
    writeFile(watched, "appeared");

    check(log.waitForCalls(1));
    check(log.calls[0].size() == 1);
    check(log.calls[0][0] == watched);

    writeFile(watched, "modified again");

    check(log.waitForCalls(2));
    check(log.calls[1].size() == 1);
    check(log.calls[1][0] == watched);
};

auto tDestroyStops = test("FileWatcher/destroyingTheWatcherStopsCallbacks") = []
{
    auto dir = TemporaryFile {};
    auto file = dir.path() / "source.cpp";
    writeFile(file, "a");

    auto log = ChangeLog {};
    auto watcher = std::optional<FileWatcher> {};
    watcher.emplace(Vector<FilePath> {dir.path()}, log.recorder(), fastOptions());

    writeFile(file, "abcd");

    auto destroyed = false;
    auto destroy = [&]
    {
        watcher.reset();
        destroyed = true;
    };
    eacp::Threads::callAfter(MS {30}, destroy);

    check(runEventLoopUntil([&] { return destroyed; }, MS {5000}));

    writeFile(file, "abcdefgh");
    pumpFor(MS {300});

    check(log.calls.empty());
};
} // namespace FileWatcherTests
