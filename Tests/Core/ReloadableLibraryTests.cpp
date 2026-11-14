#include "Common.h"
#include <eacp/Core/Plugins/ReloadableLibrary.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/TemporaryFile.h>

#include <optional>

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
        auto options = Files::DirectoryOptions {};
        options.recursive = true;
        return Files::listFiles(stagingDirectory(), options).size();
    }

    int stagingFolderCount() const
    {
        return Files::listDirectory(stagingDirectory()).size();
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
    check(library.getLoadedPath().fileName() == sandbox.builtPath().fileName());
    check(library.getLoadedPath().parentDirectory().parentDirectory()
          == sandbox.stagingDirectory());
    check(library.getLoadedPath().parentDirectory().fileName().starts_with(
        "Module-1-"));
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
    check(!Files::exists(firstCopy.parentDirectory()));
    check(sandbox.stagedFileCount() == 1);
    check(sandbox.stagingFolderCount() == 1);
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

auto tValidatorRefuses =
    test("ReloadableLibrary/aValidatorRefusalKeepsTheImage") = []
{
    auto sandbox = Sandbox {};
    auto library =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    auto refuseB = [](const eacp::Plugins::DynamicLibrary& image)
    {
        auto name = image.findFunction<NameFunction>("eacpReloadFixtureName");
        auto isB = name != nullptr && std::string {name()} == "B";
        return isB ? std::string {"B is refused"} : std::string {};
    };

    library.validate = refuseB;

    sandbox.build(fixtureA);
    check(library.reload());
    check(library.lastError().empty());

    sandbox.build(fixtureB);
    check(!library.reload());
    check(library.lastError() == "B is refused");
    check(library.getGeneration() == 1);
    check(nameOf(library) == "A");
    check(sandbox.stagedFileCount() == 1);
};

auto tDestructorCleans = test("ReloadableLibrary/destructorEmptiesStaging") = []
{
    auto sandbox = Sandbox {};
    auto library = std::optional<ReloadableLibrary> {};
    library.emplace(sandbox.builtPath(), sandbox.stagingDirectory());

    sandbox.build(fixtureA);
    check(library->reload());
    sandbox.build(fixtureB);
    check(library->reload());
    sandbox.build(fixtureA);
    check(library->reload());
    check(sandbox.stagedFileCount() >= 1);

    auto destroyInsideTurn = [&library]
    {
        check(eacp::Threads::isEventLoopRunning());
        library.reset();
    };

    eacp::Threads::runEventLoopFor(eacp::Time::MS {20}, destroyInsideTurn);

    check(sandbox.stagedFileCount() == 0);
    check(sandbox.stagingFolderCount() == 0);
};

auto tMissingSymbolExplained =
    test("ReloadableLibrary/aMissingSymbolSaysToRestart") = []
{
    using eacp::Plugins::Detail::explainedLoadError;

    auto restart = std::string {"so restart the app)"};

    for (auto error: {std::string {"error 127: The specified procedure could not "
                                   "be found."},
                      std::string {"/tmp/Module.so: undefined symbol: _Z3foov"},
                      std::string {"dlopen(/tmp/Module.so, 0x0002): symbol not "
                                   "found in flat namespace '__Z3foov'"}})
    {
        check(explainedLoadError(error).starts_with(error));
        check(explainedLoadError(error).ends_with(restart));
    }

    auto unrelated = std::string {"error 193: %1 is not a valid Win32 application."};
    check(explainedLoadError(unrelated) == unrelated);
};

auto tSymbolsStaged = test("ReloadableLibrary/symbolsAreStagedBesideTheImage") = []
{
    auto sandbox = Sandbox {};
    auto library =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    sandbox.build(fixtureA);
    check(library.reload());
    check(!Files::exists(library.getLoadedPath().withExtension(".pdb")));

    auto symbols = sandbox.builtPath().withExtension(".pdb");
    Files::writeFileAtomically(symbols, std::string_view {"symbols B"});
    sandbox.build(fixtureB);
    check(library.reload());

    auto staged = library.getLoadedPath().withExtension(".pdb");
    check(staged.parentDirectory() == library.getLoadedPath().parentDirectory());
    check(Files::readFile(staged) == "symbols B");
};

auto tSymbolsElsewhere =
    test("ReloadableLibrary/symbolsKeptElsewhereAreStagedUnderTheirOwnName") = []
{
    auto sandbox = Sandbox {};
    auto library =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};
    library.symbolsPath = sandbox.root.path() / "Symbols" / "Named.pdb";

    check(Files::createDirectories(library.symbolsPath.parentDirectory()));
    Files::writeFileAtomically(library.symbolsPath, std::string_view {"named"});
    sandbox.build(fixtureA);
    check(library.reload());

    auto staged = library.getLoadedPath().parentDirectory() / "Named.pdb";
    check(Files::readFile(staged) == "named");
};

void writeLeftover(const FilePath& file)
{
    check(Files::createDirectories(file.parentDirectory()));
    Files::writeFileAtomically(file, std::string_view {"left over"});
}

auto tSweep = test("ReloadableLibrary/copiesLeftByEarlierRunsGoOnWindows") = []
{
    auto sandbox = Sandbox {};
    auto staging = sandbox.stagingDirectory();
    auto image = sandbox.builtPath().fileName();
    // No process has id 1 on Windows (ids are multiples of 4, and 4 is System,
    // which is always running and never ours).
    auto leftover = staging / "Module-3-1-1" / image;
    auto kept = {staging / "Other-3-1-1" / image,
                 staging / "Module-3-1" / image,
                 staging / "Module-a-1-1" / image,
                 staging / "Module-3-1-1-2" / image,
                 staging / "Module-3-4-1" / image,
                 staging / "Module" / image};

    writeLeftover(leftover);
    writeLeftover(leftover.withExtension(".pdb"));

    for (const auto& file: kept)
        writeLeftover(file);

    auto library = ReloadableLibrary {sandbox.builtPath(), staging};

    check(Files::exists(leftover.parentDirectory()) != eacp::Platform::isWindows());

    for (const auto& file: kept)
        check(Files::exists(file));
};

auto tSweepSparesLoaded =
    test("ReloadableLibrary/theSweepSparesACopyAnotherLibraryHasLoaded") = []
{
    auto sandbox = Sandbox {};
    auto first = ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    sandbox.build(fixtureA);
    check(first.reload());

    auto second =
        ReloadableLibrary {sandbox.builtPath(), sandbox.stagingDirectory()};

    check(Files::exists(first.getLoadedPath()));
    check(answerOf(first) == 1);
};
} // namespace ReloadableLibraryTests
