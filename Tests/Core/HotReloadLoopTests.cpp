#include "Common.h"
#include <eacp/Core/Plugins/HotReloadLoop.h>
#include <eacp/Core/Utils/TemporaryFile.h>

#include <optional>

using namespace nano;

namespace HotReloadLoopTests
{
using eacp::FilePath;
using eacp::Plugins::BuildFailure;
using eacp::Plugins::HotReloadLoop;
using eacp::Plugins::ReloadableLibrary;
using eacp::Threads::runEventLoopUntil;
using eacp::Time::MS;
using State = HotReloadLoop::State;
namespace Files = eacp::Files;

using AnswerFunction = int (*)();

const auto fixtureA = FilePath {EACP_RELOAD_FIXTURE_A};
const auto fixtureB = FilePath {EACP_RELOAD_FIXTURE_B};
const auto fixtureC = FilePath {EACP_RELOAD_FIXTURE_C};
constexpr auto answerSymbol = "eacpReloadFixtureAnswer";

std::string quoted(const FilePath& path)
{
    return "'" + path.str() + "'";
}

int answerOf(ReloadableLibrary& library)
{
    auto answer = library.findFunction<AnswerFunction>(answerSymbol);
    return answer != nullptr ? answer() : 0;
}

void pumpFor(MS duration)
{
    auto never = [] { return false; };
    runEventLoopUntil(never, duration);
}

struct Sandbox
{
    Sandbox() { check(Files::createDirectories(root.path())); }

    FilePath sources() const { return root.path() / "Sources"; }

    FilePath modulePath() const
    {
        return root.path() / ("Module" + fixtureA.extension());
    }

    HotReloadLoop::Options options(const std::string& buildScript) const
    {
        auto loopOptions = HotReloadLoop::Options {};
        loopOptions.sources.directory = sources();
        loopOptions.sources.watcher.pollInterval = MS {20};
        loopOptions.sources.watcher.settle = MS {50};
        loopOptions.buildCommand = shellCommand(buildScript);
        loopOptions.modulePath = modulePath();
        loopOptions.stagingDirectory = root.path() / "Staging";
        loopOptions.requiredSymbol = answerSymbol;
        return loopOptions;
    }

    std::string copyScript(const FilePath& fixture) const
    {
        return "cp " + quoted(fixture) + " " + quoted(modulePath());
    }

    std::string copyInTurnScript(const FilePath& first, const FilePath& then) const
    {
        return "if [ -f " + quoted(modulePath()) + " ]; then " + copyScript(then)
               + "; else " + copyScript(first) + "; fi";
    }

    void touchSource(std::string_view contents) const
    {
        writeTestFile(eacp::toStdPath(sources() / "nested/Layout.cpp"), contents);
    }

    eacp::TemporaryFile root;
};

struct Events
{
    explicit Events(HotReloadLoop& loop)
    {
        auto started = [this] { ++builds; };
        auto reloaded = [this](ReloadableLibrary& library)
        { answers.add(answerOf(library)); };
        auto buildFailed = [this](const BuildFailure& failure)
        { buildFailures.add(failure); };
        auto loadFailed = [this](const std::string& error)
        { loadFailures.add(error); };
        auto stateChanged = [this, &loop] { states.add(loop.getState()); };

        loop.onBuildStarted = started;
        loop.onReloaded = reloaded;
        loop.onBuildFailed = buildFailed;
        loop.onLoadFailed = loadFailed;
        loop.onStateChanged = stateChanged;
    }

    int finished() const
    {
        return answers.size() + buildFailures.size() + loadFailures.size();
    }

    bool waitForFinished(int count) const
    {
        auto reached = [this, count] { return finished() >= count; };
        return runEventLoopUntil(reached, MS {10000});
    }

    int answer(int index) const
    {
        return index < answers.size() ? answers[index] : 0;
    }

    BuildFailure buildFailure(int index) const
    {
        return index < buildFailures.size() ? buildFailures[index] : BuildFailure {};
    }

    std::string loadFailure(int index) const
    {
        return index < loadFailures.size() ? loadFailures[index] : std::string {};
    }

    State state(int index) const
    {
        return index < states.size() ? states[index] : State::Idle;
    }

    int builds = 0;
    eacp::Vector<int> answers;
    eacp::Vector<BuildFailure> buildFailures;
    eacp::Vector<std::string> loadFailures;
    eacp::Vector<State> states;
};

std::string summaryOf(const std::string& output)
{
    auto result = eacp::Processes::ProcessResult {};
    result.launched = true;
    result.exited = true;
    result.exitCode = 1;
    result.output = output;
    return eacp::Plugins::buildFailureFrom(result).summary;
}

auto tChangeReloads = test("HotReloadLoop/aSourceChangeBuildsAndReloads") = []
{
    auto sandbox = Sandbox {};
    sandbox.touchSource("a");

    auto loop = HotReloadLoop {sandbox.options(sandbox.copyScript(fixtureA))};
    auto events = Events {loop};

    check(loop.getState() == State::Idle);
    check(loop.getGeneration() == 0);
    check(loop.describeState() == "generation 0");

    sandbox.touchSource("abc");

    check(events.waitForFinished(1));
    check(events.builds == 1);
    check(events.answers.size() == 1);
    check(events.answer(0) == 1);
    check(loop.getGeneration() == 1);
    check(loop.getState() == State::Loaded);
    check(loop.describeState() == "generation 1");
};

auto tNextGeneration = test("HotReloadLoop/aRebuildLoadsTheNextGeneration") = []
{
    auto sandbox = Sandbox {};
    auto script = sandbox.copyInTurnScript(fixtureA, fixtureB);

    auto loop = HotReloadLoop {sandbox.options(script)};
    auto events = Events {loop};

    loop.rebuild();
    check(events.waitForFinished(1));
    loop.rebuild();
    check(events.waitForFinished(2));

    check(events.answer(0) == 1);
    check(events.answer(1) == 2);
    check(loop.getGeneration() == 2);
};

auto tCoalescedRerun = test("HotReloadLoop/aChangeMidBuildRebuildsAsBuilding") = []
{
    auto sandbox = Sandbox {};
    auto script = "sleep 0.3; " + sandbox.copyInTurnScript(fixtureA, fixtureB);

    auto loop = HotReloadLoop {sandbox.options(script)};
    auto events = Events {loop};

    loop.rebuild();
    loop.rebuild();

    check(events.waitForFinished(2));
    check(events.builds == 2);
    check(events.states.size() == 4);
    check(events.state(0) == State::Building);
    check(events.state(1) == State::Loaded);
    check(events.state(2) == State::Building);
    check(events.state(3) == State::Loaded);
    check(events.answer(1) == 2);
};

auto tFailedBuild = test("HotReloadLoop/aFailedBuildKeepsTheGeneration") = []
{
    auto sandbox = Sandbox {};
    auto script = std::string {"echo 'compiling'; "
                               "echo 'Layout.cpp:3:1: error: expected ;'; "
                               "echo '1 error generated.'; exit 1"};

    auto loop = HotReloadLoop {sandbox.options(script)};
    auto events = Events {loop};

    loop.rebuild();

    check(events.waitForFinished(1));
    check(events.buildFailures.size() == 1);
    check(events.buildFailure(0).launched);
    check(events.buildFailure(0).exited);
    check(events.buildFailure(0).exitCode == 1);
    check(events.buildFailure(0).summary == "Layout.cpp:3:1: error: expected ;");
    check(loop.getGeneration() == 0);
    check(loop.getState() == State::BuildFailed);
    check(loop.describeState() == "build failed: Layout.cpp:3:1: error: expected ;");
};

auto tFailedLoad = test("HotReloadLoop/anUnloadableModuleKeepsTheGeneration") = []
{
    auto sandbox = Sandbox {};
    auto script = "echo junk > " + quoted(sandbox.modulePath());

    auto loop = HotReloadLoop {sandbox.options(script)};
    auto events = Events {loop};

    loop.rebuild();

    check(events.waitForFinished(1));
    check(events.loadFailures.size() == 1);
    check(!events.loadFailure(0).empty());
    check(loop.getGeneration() == 0);
    check(loop.getState() == State::LoadFailed);
    check(loop.describeState().starts_with("load failed: "));
};

auto tMissingSymbol =
    test("HotReloadLoop/aModuleWithoutTheRequiredSymbolKeepsThePrevious") = []
{
    auto sandbox = Sandbox {};
    auto script = sandbox.copyInTurnScript(fixtureA, fixtureC);

    auto loop = HotReloadLoop {sandbox.options(script)};
    auto events = Events {loop};

    loop.rebuild();
    check(events.waitForFinished(1));
    loop.rebuild();
    check(events.waitForFinished(2));

    check(events.answers.size() == 1);
    check(events.loadFailure(0) == "no eacpReloadFixtureAnswer");
    check(loop.getGeneration() == 1);
    check(answerOf(loop.getLibrary()) == 1);
    check(loop.describeState() == "load failed: no eacpReloadFixtureAnswer");
};

auto tDestroyedMidBuild = test("HotReloadLoop/destroyingMidBuildCallsNothing") = []
{
    auto sandbox = Sandbox {};
    auto loop = std::optional<HotReloadLoop> {};
    loop.emplace(sandbox.options("sleep 1; " + sandbox.copyScript(fixtureA)));
    auto events = Events {*loop};

    loop->rebuild();
    check(events.builds == 1);

    loop.reset();
    pumpFor(MS {300});

    check(events.finished() == 0);
    check(events.states.size() == 1);
};

auto tExcludesOutputs =
    test("HotReloadLoop/outputsInsideTheSourcesAreNotWatched") = []
{
    auto sandbox = Sandbox {};
    auto options = sandbox.options("true");
    options.stagingDirectory = sandbox.sources() / "Staging";

    auto loop = HotReloadLoop {options};
    auto events = Events {loop};

    writeTestFile(eacp::toStdPath(options.stagingDirectory / "Copy.cpp"), "x");
    pumpFor(MS {300});

    check(events.builds == 0);
};

auto tClangSummary = test("HotReloadLoop/summaryPrefersALocatedClangError") = []
{
    auto output = "[1/2] Building CXX object Layout.cpp.o\n"
                  "FAILED: Layout.cpp.o\n"
                  "\x1b[1m/src/Layout.cpp:14:32: \x1b[0m\x1b[0;1;31merror: "
                  "\x1b[0mexpected ';' after top level declarator\x1b[0m\n"
                  "1 error generated.\n"
                  "ninja: build stopped: subcommand failed.\n";

    check(summaryOf(output)
          == "/src/Layout.cpp:14:32: error: expected ';' after top level "
             "declarator");
};

auto tGccSummary = test("HotReloadLoop/summaryPrefersALocatedGccError") = []
{
    auto output = "Layout.cpp: In function 'void demoLayout()':\n"
                  "Layout.cpp:3:5: error: 'x' was not declared in this scope\n"
                  "    3 |     x = 1;\n";

    check(summaryOf(output)
          == "Layout.cpp:3:5: error: 'x' was not declared in this scope");
};

auto tMsvcSummary = test("HotReloadLoop/summaryReadsAnMsvcError") = []
{
    auto output = "Layout.cpp\n"
                  "C:\\src\\Layout.cpp(12): error C2065: 'x': undeclared "
                  "identifier\n";

    check(summaryOf(output)
          == "C:\\src\\Layout.cpp(12): error C2065: 'x': undeclared identifier");
};

auto tLldSummary = test("HotReloadLoop/summarySkipsTheDriverLine") = []
{
    auto output = "ld.lld: error: undefined symbol: foo()\n"
                  ">>> referenced by Layout.cpp:7\n"
                  "clang++: error: linker command failed with exit code 1\n";

    check(summaryOf(output) == "ld.lld: error: undefined symbol: foo()");
};

auto tCMakeSummary = test("HotReloadLoop/summaryReadsACMakeError") = []
{
    auto output = "-- Configuring incomplete\n"
                  "CMake Error at CMakeLists.txt:4 (add_library):\n"
                  "  Cannot find source file: Missing.cpp\n"
                  "-- Generating done\n";

    check(summaryOf(output) == "CMake Error at CMakeLists.txt:4 (add_library):");
};

auto tNinjaSummary = test("HotReloadLoop/summaryFallsBackToNinjasFailedLine") = []
{
    auto output = "[1/2] Linking CXX shared module Layout.so\n"
                  "FAILED: Layout.so\n"
                  "ld: symbol(s) not found for architecture arm64\n"
                  "ninja: build stopped: subcommand failed.\n";

    check(summaryOf(output) == "FAILED: Layout.so");
};

auto tLastLineSummary = test("HotReloadLoop/summaryFallsBackToTheLastLine") = []
{
    check(summaryOf("step one\nninja: error: loading 'build.ninja'\n\n")
          == "ninja: error: loading 'build.ninja'");
};

auto tNoOutputSummary = test("HotReloadLoop/summaryWithoutOutputNamesTheCause") = []
{
    auto notStarted = eacp::Processes::ProcessResult {};
    auto cancelled = eacp::Processes::ProcessResult {};
    cancelled.launched = true;

    auto failure = eacp::Plugins::buildFailureFrom(notStarted);

    check(failure.summary == "build command did not start");
    check(!failure.launched);
    check(failure.exitCode == -1);
    check(eacp::Plugins::buildFailureFrom(cancelled).summary == "build cancelled");
};
} // namespace HotReloadLoopTests
