#include "Common.h"

using namespace nano;

namespace SourceWatcherTests
{
using eacp::FilePath;
using eacp::TemporaryFile;
using eacp::Vector;
using eacp::Files::FileWatcher;
using eacp::Files::SourceWatcher;
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

SourceWatcher::Options fastOptions(const FilePath& directory)
{
    auto options = SourceWatcher::Options {};
    options.directory = directory;
    options.watcher.pollInterval = MS {20};
    options.watcher.settle = MS {50};
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

auto tNestedSources = test("SourceWatcher/reportsSourcesInNestedFolders") = []
{
    auto dir = TemporaryFile {};
    auto source = dir.path() / "src/deep/Layout.cpp";
    auto header = dir.path() / "src/deep/Layout.h";
    writeFile(source, "a");
    writeFile(header, "a");

    auto log = ChangeLog {};
    auto watcher = SourceWatcher {fastOptions(dir.path()), log.recorder()};

    check(watcher.getDirectory() == dir.path());

    writeFile(source, "abc");
    writeFile(header, "abc");

    check(log.waitForCalls(1));
    pumpFor(MS {200});

    check(log.calls.size() == 1);
    check(log.calls[0].size() == 2);
    check(contains(log.calls[0], source));
    check(contains(log.calls[0], header));
};

auto tIgnoresOtherFiles = test("SourceWatcher/ignoresFilesThatAreNotSource") = []
{
    auto dir = TemporaryFile {};
    auto notes = dir.path() / "nested/notes.txt";
    auto source = dir.path() / "nested/main.cpp";

    auto log = ChangeLog {};
    auto watcher = SourceWatcher {fastOptions(dir.path()), log.recorder()};

    writeFile(notes, "ignored");
    pumpFor(MS {300});
    check(log.calls.empty());

    writeFile(source, "int main() {}");

    check(log.waitForCalls(1));
    check(log.calls[0].size() == 1);
    check(log.calls[0][0] == source);
};

auto tCustomPattern = test("SourceWatcher/honoursACustomPattern") = []
{
    auto dir = TemporaryFile {};
    auto shader = dir.path() / "Blur.glsl";
    auto source = dir.path() / "Blur.cpp";

    auto options = fastOptions(dir.path());
    options.pattern = "*.glsl";

    auto log = ChangeLog {};
    auto watcher = SourceWatcher {options, log.recorder()};

    writeFile(source, "ignored");
    pumpFor(MS {300});
    check(log.calls.empty());

    writeFile(shader, "void main() {}");

    check(log.waitForCalls(1));
    check(log.calls[0].size() == 1);
    check(log.calls[0][0] == shader);
};

auto tExcluded = test("SourceWatcher/ignoresExcludedDirectories") = []
{
    auto dir = TemporaryFile {};
    auto excluded = dir.path() / "build";
    auto generated = excluded / "deep/Generated.cpp";
    auto source = dir.path() / "src/Main.cpp";

    auto options = fastOptions(dir.path());
    options.watcher.excludedDirectories = {excluded};

    auto log = ChangeLog {};
    auto watcher = SourceWatcher {options, log.recorder()};

    writeFile(generated, "ignored");
    pumpFor(MS {300});
    check(log.calls.empty());

    writeFile(source, "int main() {}");

    check(log.waitForCalls(1));
    check(log.calls.size() == 1);
    check(log.calls[0].size() == 1);
    check(log.calls[0][0] == source);
};

auto tLateDirectory = test("SourceWatcher/watchesADirectoryCreatedLater") = []
{
    auto parent = TemporaryFile {};
    auto dir = parent.path() / "later";
    auto source = dir / "Late.h";

    auto log = ChangeLog {};
    auto watcher = SourceWatcher {dir, log.recorder()};

    pumpFor(MS {100});
    writeFile(source, "#pragma once");

    check(log.waitForCalls(1));
    check(log.calls[0].size() == 1);
    check(log.calls[0][0] == source);
};
} // namespace SourceWatcherTests
