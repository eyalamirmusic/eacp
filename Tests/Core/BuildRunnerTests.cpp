#include "Common.h"
#include "HarnessScript.h"

using namespace nano;

namespace BuildRunnerTests
{
namespace Proc = eacp::Processes;
using eacp::Threads::runEventLoopUntil;

Proc::ProcessOptions sleepCommand(int milliseconds)
{
    return HarnessScript {}.sleep(milliseconds).options();
}

struct Recorder
{
    explicit Recorder(Proc::BuildRunner& runner)
    {
        auto recordChunk = [this](std::string_view chunk)
        {
            chunksOnMainThread = chunksOnMainThread && eacp::Threads::isMainThread();
            output += chunk;
        };

        auto recordFinish = [this](const Proc::ProcessResult& result)
        {
            finishedOnMainThread =
                finishedOnMainThread && eacp::Threads::isMainThread();
            outputAtFinish = output;
            results.push_back(result);
        };

        runner.onOutput = recordChunk;
        runner.onFinished = recordFinish;
    }

    bool waitForFinishes(int count, int timeoutMs = 5000)
    {
        auto done = [this, count] { return (int) results.size() >= count; };
        return runEventLoopUntil(done, eacp::Time::MS {timeoutMs});
    }

    std::string output;
    std::string outputAtFinish;
    eacp::Vector<Proc::ProcessResult> results;
    bool chunksOnMainThread = true;
    bool finishedOnMainThread = true;
};

auto tSucceeds = test("BuildRunner/succeedingCommandStreamsOutput") = []
{
    auto runner =
        Proc::BuildRunner {HarnessScript {}.print("one").print("two").options()};
    auto recorder = Recorder {runner};

    runner.run();
    check(runner.isRunning());
    check(recorder.waitForFinishes(1));

    check(recorder.results.size() == 1);
    check(recorder.results[0].launched);
    check(recorder.results[0].exited);
    check(recorder.results[0].exitCode == 0);
    check(recorder.output == "one\ntwo\n");
    check(recorder.outputAtFinish == "one\ntwo\n");
    check(recorder.results[0].output == "one\ntwo\n");
    check(recorder.chunksOnMainThread);
    check(recorder.finishedOnMainThread);
    check(!runner.isRunning());

    runEventLoopUntil([] { return false; }, eacp::Time::MS {100});
    check(recorder.results.size() == 1);
};

auto tFailingExitCode = test("BuildRunner/failingCommandReportsExitCode") = []
{
    auto runner = Proc::BuildRunner {HarnessScript {}.exit(3).options()};
    auto recorder = Recorder {runner};

    runner.run();
    check(recorder.waitForFinishes(1));

    check(recorder.results[0].exited);
    check(recorder.results[0].exitCode == 3);
};

auto tMergesStderr = test("BuildRunner/stderrIsMergedIntoTheStream") = []
{
    auto script = HarnessScript {}.print("out").printError("err").print("end");
    auto runner = Proc::BuildRunner {script.options()};
    auto recorder = Recorder {runner};

    runner.run();
    check(recorder.waitForFinishes(1));

    check(recorder.output == "out\nerr\nend\n");
    check(recorder.results[0].errorOutput.empty());
};

auto tCoalesces = test("BuildRunner/runsDuringARunCoalesceIntoOne") = []
{
    auto runner = Proc::BuildRunner {sleepCommand(300)};
    auto recorder = Recorder {runner};

    runner.run();
    check(!runner.isRerunPending());

    runner.run();
    runner.run();
    runner.run();
    check(runner.isRerunPending());

    check(recorder.waitForFinishes(1));
    check(runner.isRunning());
    check(!runner.isRerunPending());

    check(recorder.waitForFinishes(2));
    runEventLoopUntil([] { return false; }, eacp::Time::MS {500});

    check(recorder.results.size() == 2);
    check(!runner.isRunning());
};

auto tCancel = test("BuildRunner/cancelFinishesPromptlyWithoutRerun") = []
{
    auto runner = Proc::BuildRunner {sleepCommand(10000)};
    auto recorder = Recorder {runner};

    runner.run();
    runner.run();
    runEventLoopUntil([] { return false; }, eacp::Time::MS {100});

    auto started = std::chrono::steady_clock::now();
    runner.cancel();
    check(!runner.isRerunPending());
    check(recorder.waitForFinishes(1, 3000));
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds {2});

    check(recorder.results[0].launched);
    check(!recorder.results[0].exited);

    runEventLoopUntil([] { return false; }, eacp::Time::MS {300});
    check(recorder.results.size() == 1);
    check(!runner.isRunning());
};

void checkDestroyingDuringARunReturnsPromptly(const HarnessScript& script)
{
    auto calls = 0;

    auto started = std::chrono::steady_clock::now();
    {
        auto runner = Proc::BuildRunner {script.options()};
        runner.onOutput = [&calls](std::string_view) { ++calls; };
        runner.onFinished = [&calls](const Proc::ProcessResult&) { ++calls; };

        runner.run();
        runner.run();
        std::this_thread::sleep_for(std::chrono::milliseconds {300});
    }
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds {2});

    runEventLoopUntil([] { return false; }, eacp::Time::MS {300});
    check(calls == 0);
}

auto tDestroy = test("BuildRunner/destroyingDuringARunReturnsPromptly") = []
{
    checkDestroyingDuringARunReturnsPromptly(
        HarnessScript {}.print("hi").sleep(10000));
};

// What `cmake --build` does: the command starts a build tool that writes to the
// same output, and the tool must end with it.
auto tDestroyTree =
    test("BuildRunner/destroyingDuringARunEndsWhatTheCommandStarted") = []
{
    auto script = HarnessScript {}.print("hi").spawnSleeping(10000).sleep(10000);
    checkDestroyingDuringARunReturnsPromptly(script);
};

auto tCancelTree = test("BuildRunner/cancelEndsWhatTheCommandStarted") = []
{
    auto script = HarnessScript {}.spawnSleeping(10000).sleep(10000);
    auto runner = Proc::BuildRunner {script.options()};
    auto recorder = Recorder {runner};

    runner.run();
    runEventLoopUntil([] { return false; }, eacp::Time::MS {200});

    auto started = std::chrono::steady_clock::now();
    runner.cancel();
    check(recorder.waitForFinishes(1, 3000));
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds {2});
    check(recorder.results.size() == 1 && !recorder.results[0].exited);
};
} // namespace BuildRunnerTests
