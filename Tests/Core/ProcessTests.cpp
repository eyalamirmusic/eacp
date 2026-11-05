#include "Common.h"
#include <csignal>
#include <mutex>

#include <sys/types.h>
#include <unistd.h>

using namespace nano;
namespace Proc = eacp::Processes;

auto tCapturesStdout = test("Process/run/capturesStdout") = []
{
    auto result = Proc::run("/bin/echo", {"hello", "world"});

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);
    check(result.output == "hello world\n");
    check(result.errorOutput.empty());
};

auto tCapturesStderr = test("Process/run/capturesStderr") = []
{
    auto result = Proc::run("/bin/sh", {"-c", "echo oops 1>&2"});

    check(result.exitCode == 0);
    check(result.errorOutput == "oops\n");
    check(result.output.empty());
};

auto tNonZeroExit = test("Process/run/nonZeroExit") = []
{
    auto result = Proc::run("/bin/sh", {"-c", "exit 7"});

    check(result.launched);
    check(result.exitCode == 7);
};

auto tMissingExecutableExits127 = test("Process/run/missingExecutable") = []
{
    auto result = Proc::run("/no/such/binary-eacp", {});

    // fork() succeeds, execvp() fails, the child exits 127.
    check(result.exitCode == 127);
};

auto tEnvironmentOverride = test("Process/run/environmentOverride") = []
{
    auto options = Proc::ProcessOptions {"/bin/sh",
                                         {"-c", "printf %s \"$EACP_TEST_VAR\""},
                                         {},
                                         {{"EACP_TEST_VAR", "42"}}};

    auto result = Proc::run(std::move(options));

    check(result.output == "42");
};

auto tStdinRoundTrip = test("Process/stdin/roundTrip") = []
{
    auto process = Proc::Process {"/bin/cat", {}};
    check(process.launched());

    check(process.writeToInput("ping\n"));
    process.closeInput();

    auto code = process.wait();

    check(code == 0);
    check(process.output() == "ping\n");
};

auto tTerminateStopsChild = test("Process/terminate/stopsRunningChild") = []
{
    auto process = Proc::Process {"/bin/sleep", {"30"}};
    check(process.launched());
    check(process.isRunning());

    process.terminate();
    auto code = process.wait();

    check(!process.isRunning());
    check(code == 128 + 15); // terminated by SIGTERM
};

auto tRunAsyncResolves = test("Process/runAsync/resolvesOnMainThread") = []
{
    auto async =
        Proc::runAsync(Proc::ProcessOptions {"/bin/echo", {"async"}, {}, {}});

    auto result = async.waitFor(eacp::Time::MS {5000});

    check(result.exitCode == 0);
    check(result.output == "async\n");
};

auto tDetachedSurvivesDestruction = test("Process/detached/survivesDestruction") = []
{
    auto pid = long {-1};

    {
        auto options = Proc::ProcessOptions {"/bin/sleep", {"30"}};
        options.detached = true;

        auto process = Proc::Process {std::move(options)};
        check(process.launched());
        check(process.isRunning());
        pid = process.id();
    }

    // Destroying the Process must leave a detached child running.
    check(::kill((pid_t) pid, 0) == 0);
    ::kill((pid_t) pid, SIGKILL);
};

namespace
{
Proc::ProcessOptions bothStreams()
{
    return {"/bin/sh", {"-c", "echo one; echo two 1>&2; echo three"}};
}

std::string withoutTrailingNewline(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    return text;
}
} // namespace

auto tWorkingDirectory = test("Process/run/workingDirectory") = []
{
    auto dir = scratchDirectory("process-working-directory");

    auto options = Proc::ProcessOptions {"pwd"};
    options.workingDirectory = dir.string();

    auto result = Proc::run(std::move(options));

    check(result.exitCode == 0);
    check(std::filesystem::canonical(withoutTrailingNewline(result.output))
          == std::filesystem::canonical(dir));

    std::filesystem::remove_all(dir);
};

auto tRunClosesInput = test("Process/run/closesInput") = []
{
    // cat waits on stdin until end-of-file; run() must not leave it open.
    auto result = Proc::run("/bin/cat");

    check(result.exited);
    check(result.exitCode == 0);
    check(result.output.empty());
};

auto tSeparateStreams = test("Process/run/separateStreams") = []
{
    auto result = Proc::run(bothStreams());

    check(result.exitCode == 0);
    check(result.output == "one\nthree\n");
    check(result.errorOutput == "two\n");
};

auto tMergedStreams = test("Process/run/mergedStreams") = []
{
    auto options = bothStreams();
    options.mergeErrorOutput = true;

    auto result = Proc::run(std::move(options));

    check(result.exitCode == 0);
    check(result.output == "one\ntwo\nthree\n");
    check(result.errorOutput.empty());
};

auto tOnOutputSeesEveryChunk = test("Process/run/onOutputSeesEveryChunk") = []
{
    auto mutex = std::mutex {};
    auto seen = std::string {};

    auto options = bothStreams();
    options.mergeErrorOutput = true;
    options.onOutput = [&](std::string_view chunk)
    {
        auto lock = std::lock_guard {mutex};
        seen.append(chunk);
    };

    auto result = Proc::run(std::move(options));

    auto lock = std::lock_guard {mutex};
    check(seen == "one\ntwo\nthree\n");
    check(seen == result.output);
};

auto tOnOutputSkipsSeparateStderr =
    test("Process/run/onOutputSkipsSeparateStderr") = []
{
    auto mutex = std::mutex {};
    auto seen = std::string {};

    auto options = bothStreams();
    options.onOutput = [&](std::string_view chunk)
    {
        auto lock = std::lock_guard {mutex};
        seen.append(chunk);
    };

    auto result = Proc::run(std::move(options));

    auto lock = std::lock_guard {mutex};
    check(seen == "one\nthree\n");
    check(result.errorOutput == "two\n");
};

// No shell on POSIX: the line is split JUCE's way, so the quotes reach the
// child as part of its arguments and only the executable is unquoted.
auto tCommandLineKeepsQuotes =
    test("Process/commandLine/keepsQuotesInArguments") = []
{
    auto options = Proc::ProcessOptions {};
    options.commandLine = "\"/bin/echo\"  \"hello world\" plain";

    auto result = Proc::run(std::move(options));

    check(result.launched);
    check(result.exitCode == 0);
    check(result.output == "\"hello world\" plain\n");
};

auto tCommandLineSearchesPath = test("Process/commandLine/searchesPath") = []
{
    auto options = Proc::ProcessOptions {};
    options.commandLine = "echo from-path";

    auto result = Proc::run(std::move(options));

    check(result.exitCode == 0);
    check(result.output == "from-path\n");
};

auto tCommandLineIgnoresExecutable =
    test("Process/commandLine/replacesExecutableAndArguments") = []
{
    auto options = Proc::ProcessOptions {"/bin/sh", {"-c", "exit 3"}};
    options.commandLine = "/bin/echo replaced";

    auto result = Proc::run(std::move(options));

    check(result.exitCode == 0);
    check(result.output == "replaced\n");
};

auto tBlankCommandLineDoesNotLaunch =
    test("Process/commandLine/blankDoesNotLaunch") = []
{
    auto options = Proc::ProcessOptions {"/bin/echo", {"unused"}};
    options.commandLine = " \t ";

    auto result = Proc::run(std::move(options));

    check(!result.launched);
};

// HOME and PATH are set in any environment the tests run in; the tests read
// them rather than set variables of their own.
auto tRemovedEnvironment = test("Process/run/removedEnvironment") = []
{
    auto options = Proc::ProcessOptions {};
    options.executable = "/bin/sh";
    options.arguments = {"-c", "printf '%s|%s' \"${HOME-unset}\" \"${PATH:+kept}\""};
    options.removedEnvironment = {"HOME"};

    auto result = Proc::run(std::move(options));

    check(result.output == "unset|kept");
};

auto tRemovedThenOverridden = test("Process/run/removedThenOverridden") = []
{
    auto options = Proc::ProcessOptions {};
    options.executable = "/bin/sh";
    options.arguments = {"-c", "printf %s \"$HOME\""};
    options.removedEnvironment = {"HOME"};
    options.environment = {{"HOME", "override"}};

    auto result = Proc::run(std::move(options));

    check(result.output == "override");
};

auto tInheritErrorOutput = test("Process/run/inheritErrorOutput") = []
{
    int errorPipe[2] = {-1, -1};
    check(::pipe(errorPipe) == 0);

    auto savedError = ::dup(STDERR_FILENO);
    ::dup2(errorPipe[1], STDERR_FILENO);
    ::close(errorPipe[1]);

    auto options = Proc::ProcessOptions {};
    options.executable = "/bin/sh";
    options.arguments = {"-c", "echo out; echo err 1>&2"};
    options.inheritErrorOutput = true;

    auto result = Proc::run(std::move(options));

    ::dup2(savedError, STDERR_FILENO);
    ::close(savedError);

    char buffer[16] = {};
    auto count = ::read(errorPipe[0], buffer, sizeof buffer);
    ::close(errorPipe[0]);

    check(result.output == "out\n");
    check(result.errorOutput.empty());
    check(count > 0 && std::string(buffer, (size_t) count) == "err\n");
};

auto tMakeChildEnvironment = test("Process/makeChildEnvironment") = []
{
    auto options = Proc::ProcessOptions {};
    options.removedEnvironment = {"HOME"};
    options.environment = {{"EACP_CHILD_ONLY", "1"}};

    auto entries = Proc::makeChildEnvironment(options);
    auto hasHome = false;
    auto hasOverride = false;

    for (const auto& entry: entries)
    {
        hasHome = hasHome || entry.starts_with("HOME=");
        hasOverride = hasOverride || entry == "EACP_CHILD_ONLY=1";
    }

    check(!hasHome);
    check(hasOverride);
};
