#include "Common.h"
#include <eacp/Core/Plugins/ReloadableLibrary.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/TemporaryFile.h>

using namespace nano;

namespace ReloadableLibraryTests
{
using eacp::FilePath;
using eacp::Plugins::ReloadableLibrary;
namespace Files = eacp::Files;

using AnswerFunction = int (*)();
using NameFunction = const char* (*)();

const auto fixtureA = FilePath {EACP_RELOAD_FIXTURE_A};
const auto fixtureB = FilePath {EACP_RELOAD_FIXTURE_B};

struct Sandbox
{
    FilePath builtPath() const
    {
        return root.path() / ("Module" + fixtureA.extension());
    }

    FilePath stagingDirectory() const { return root.path() / "Staging"; }

    void build(const FilePath& fixture) const
    {
        Files::removeFile(builtPath());
        check(Files::copyFile(fixture, builtPath()));
    }

    int stagedFileCount() const
    {
        return Files::listFiles(stagingDirectory()).size();
    }

    eacp::TemporaryFile root {""};
};

int answerOf(const ReloadableLibrary& library)
{
    auto answer = library.findFunction<AnswerFunction>("eacpReloadFixtureAnswer");
    return answer != nullptr ? answer() : 0;
}

std::string nameOf(const ReloadableLibrary& library)
{
    auto name = library.findFunction<NameFunction>("eacpReloadFixtureName");
    return name != nullptr ? name() : "";
}

auto tClosedBeforeReload = test("ReloadableLibrary/closedBeforeFirstReload") = []
{
    auto sandbox = Sandbox {};
    auto library =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    check(!library.isOpen());
    check(library.getGeneration() == 0);
    check(library.getLoadedPath().empty());
    check(library.getBuiltPath() == sandbox.builtPath());
    check(library.findFunction<AnswerFunction>("eacpReloadFixtureAnswer")
          == nullptr);
};

auto tFirstReload = test("ReloadableLibrary/firstReloadOpensAStagedCopy") = []
{
    auto sandbox = Sandbox {};
    sandbox.build(fixtureA);

    auto library =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    check(library.reload());
    check(library.isOpen());
    check(library.getGeneration() == 1);
    check(answerOf(library) == 1);
    check(nameOf(library) == "A");
    check(library.getLoadedPath() != sandbox.builtPath());
    check(library.getLoadedPath().parentDirectory() == sandbox.stagingDirectory());
    check(Files::isFile(library.getLoadedPath()));
};

auto tSecondReload = test("ReloadableLibrary/reloadPicksUpARebuild") = []
{
    auto sandbox = Sandbox {};
    auto library =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    sandbox.build(fixtureA);
    check(library.reload());
    auto firstCopy = library.getLoadedPath();

    sandbox.build(fixtureB);
    check(library.reload());
    check(library.getGeneration() == 2);
    check(answerOf(library) == 2);
    check(nameOf(library) == "B");
    check(library.getLoadedPath() != firstCopy);

    eacp::Threads::runEventLoopFor(eacp::Time::MS {10});

    check(!Files::exists(firstCopy));
    check(sandbox.stagedFileCount() == 1);
};

auto tMissingBuild = test("ReloadableLibrary/missingBuildKeepsTheImage") = []
{
    auto sandbox = Sandbox {};
    auto library =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    check(!library.reload());
    check(library.getGeneration() == 0);

    sandbox.build(fixtureA);
    check(library.reload());

    Files::removeFile(sandbox.builtPath());
    check(!library.reload());
    check(library.getGeneration() == 1);
    check(answerOf(library) == 1);
};

auto tGarbageBuild = test("ReloadableLibrary/unloadableBuildKeepsTheImage") = []
{
    auto sandbox = Sandbox {};
    auto library =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    sandbox.build(fixtureA);
    check(library.reload());
    auto loaded = library.getLoadedPath();

    writeTestFile(eacp::toStdPath(sandbox.builtPath()), "not a library");

    check(!library.reload());
    check(library.isOpen());
    check(library.getGeneration() == 1);
    check(library.getLoadedPath() == loaded);
    check(answerOf(library) == 1);
    check(sandbox.stagedFileCount() == 1);
};

auto tReloadWhileLooping = test("ReloadableLibrary/reloadDefersTheOldUnmap") = []
{
    auto sandbox = Sandbox {};
    auto library =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    sandbox.build(fixtureA);
    check(library.reload());

    auto oldAnswer = library.findFunction<AnswerFunction>("eacpReloadFixtureAnswer");
    check(oldAnswer != nullptr);

    auto reloaded = false;
    auto oldStillMapped = false;

    auto reloadInsideTurn = [&]
    {
        check(eacp::Threads::isEventLoopRunning());

        sandbox.build(fixtureB);
        reloaded = library.reload();

        oldStillMapped = oldAnswer() == 1;
    };

    eacp::Threads::runEventLoopFor(eacp::Time::MS {20}, reloadInsideTurn);

    check(reloaded);
    check(oldStillMapped);
    check(library.getGeneration() == 2);
    check(answerOf(library) == 2);
};

auto tDestructorCleans = test("ReloadableLibrary/destructorEmptiesStaging") = []
{
    auto sandbox = Sandbox {};

    {
        auto library =
            ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

        sandbox.build(fixtureA);
        check(library.reload());
        sandbox.build(fixtureB);
        check(library.reload());
        sandbox.build(fixtureA);
        check(library.reload());
        check(sandbox.stagedFileCount() >= 1);
    }

    check(sandbox.stagedFileCount() == 0);
};
} // namespace ReloadableLibraryTests
