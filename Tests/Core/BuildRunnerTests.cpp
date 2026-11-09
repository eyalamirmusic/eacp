#include "Common.h"

using namespace nano;

namespace BuildRunnerTests
{
namespace Proc = eacp::Processes;
using eacp::Threads::runEventLoopUntil;

Proc::ProcessOptions sleepCommand(const std::string& seconds)
{
    auto options = Proc::ProcessOptions {};
    options.executable = "/bin/sleep";
    options.arguments = {seconds};
    return options;
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
    auto runner = Proc::BuildRunner {shellCommand("echo one; echo two")};
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
    auto runner = Proc::BuildRunner {shellCommand("exit 3")};
    auto recorder = Recorder {runner};

    runner.run();
    check(recorder.waitForFinishes(1));

    check(recorder.results[0].exited);
    check(recorder.results[0].exitCode == 3);
};

auto tMergesStderr = test("BuildRunner/stderrIsMergedIntoTheStream") = []
{
    auto runner =
        Proc::BuildRunner {shellCommand("echo out; echo err 1>&2; echo end")};
    auto recorder = Recorder {runner};

    runner.run();
    check(recorder.waitForFinishes(1));

    check(recorder.output == "out\nerr\nend\n");
    check(recorder.results[0].errorOutput.empty());
};

auto tCoalesces = test("BuildRunner/runsDuringARunCoalesceIntoOne") = []
{
    auto runner = Proc::BuildRunner {sleepCommand("0.3")};
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
    auto runner = Proc::BuildRunner {sleepCommand("10")};
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

auto tDestroy = test("BuildRunner/destroyingDuringARunReturnsPromptly") = []
{
    auto calls = 0;

    auto started = std::chrono::steady_clock::now();
    {
        auto runner = Proc::BuildRunner {shellCommand("echo hi; exec sleep 10")};
        runner.onOutput = [&calls](std::string_view) { ++calls; };
        runner.onFinished = [&calls](const Proc::ProcessResult&) { ++calls; };

        runner.run();
        runner.run();
        std::this_thread::sleep_for(std::chrono::milliseconds {100});
    }
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds {2});

    runEventLoopUntil([] { return false; }, eacp::Time::MS {300});
    check(calls == 0);
};
} // namespace BuildRunnerTests
