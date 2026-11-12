#include "Common.h"

#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace nano;
namespace Proc = eacp::Processes;
namespace fs = std::filesystem;

namespace
{
const auto harness = fs::path {EACP_MODULE_INFO_HARNESS};

struct ModuleInfoReport
{
    std::string role;
    std::string application;
};

ModuleInfoReport parseModuleInfoReport(const std::string& output)
{
    auto report = ModuleInfoReport {};
    const auto newline = output.find('\n');

    report.role = output.substr(0, newline);

    if (newline != std::string::npos)
    {
        report.application = output.substr(newline + 1);

        if (!report.application.empty() && report.application.back() == '\n')
            report.application.pop_back();
    }

    return report;
}

ModuleInfoReport runModuleInfoHarness(Proc::ProcessOptions options)
{
    const auto result = Proc::run(std::move(options));

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);

    return parseModuleInfoReport(result.output);
}

ModuleInfoReport runModuleInfoHarnessAs(const std::string& executable,
                                        const std::string& workingDirectory = {},
                                        eacp::Vector<std::string> arguments = {})
{
    auto options = Proc::ProcessOptions {};
    options.executable = executable;
    options.workingDirectory = workingDirectory;
    options.arguments = std::move(arguments);

    return runModuleInfoHarness(std::move(options));
}

// Processes::run always passes the executable as argv[0]; a launcher that
// rewrites it (exec -a, login shells, wrappers) needs posix_spawn directly.
ModuleInfoReport runModuleInfoHarnessWithArgv0(std::string argv0)
{
    const auto output =
        scratchDirectory("module-info-argv0-" + std::to_string((long) ::getpid()))
        / "output.txt";

    auto actions = posix_spawn_file_actions_t {};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    char* arguments[] = {argv0.data(), nullptr};
    auto child = pid_t {};
    const auto spawned =
        posix_spawn(&child, harness.c_str(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);

    check(spawned == 0);

    if (spawned != 0)
        return {};

    auto status = 0;
    check(waitpid(child, &status, 0) == child);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    return parseModuleInfoReport(readTestFile(output));
}

// The role is decided from images on every platform. The path is checked on
// Linux, where it used to be derived from argv[0]; macOS takes it from dyld.
void checkModuleInfoStandalone(const ModuleInfoReport& report)
{
    check(report.role == "standalone");

    if constexpr (eacp::Platform::isLinux())
    {
        auto ec = std::error_code {};
        check(fs::equivalent(fs::path {report.application}, harness, ec));
    }
}
} // namespace

// Spellings of one binary. A standalone executable is standalone whichever was
// typed. Linux used to compare /proc/self/exe with the path dladdr reports,
// which for the main program is argv[0], made absolute against the working
// directory of the moment: `./harness` (before #50), a bare name found on PATH,
// a rewritten argv[0] and a relative launch followed by a chdir all compared
// unequal, run<T>() handed the event loop to a host that was not there and
// returned, and the process exited at once with status 0.
auto tAbsolutePath = test("ModuleInfo/standaloneWhenLaunchedByAbsolutePath") = []
{ checkModuleInfoStandalone(runModuleInfoHarnessAs(harness.string())); };

auto tRelativePath = test("ModuleInfo/standaloneWhenLaunchedByRelativePath") = []
{
    checkModuleInfoStandalone(runModuleInfoHarnessAs(
        "./" + harness.filename().string(), harness.parent_path().string()));
};

auto tSymlink = test("ModuleInfo/standaloneWhenLaunchedThroughSymlink") = []
{
    auto ec = std::error_code {};

    // Named per process: NanoTest gives every case its own, and ctest runs them
    // in parallel.
    const auto link =
        fs::temp_directory_path()
        / ("eacp-module-info-harness-" + std::to_string((long) ::getpid()));

    fs::remove(link, ec);
    fs::create_symlink(harness, link, ec);
    check(!ec);

    checkModuleInfoStandalone(runModuleInfoHarnessAs(link.string()));

    fs::remove(link, ec);
};

auto tBareName = test("ModuleInfo/standaloneWhenLaunchedByBareNameFromPath") = []
{
    auto options = shellCommand(harness.filename().string());
    options.workingDirectory = fs::temp_directory_path().string();
    options.environment = {{"PATH", harness.parent_path().string()}};

    checkModuleInfoStandalone(runModuleInfoHarness(std::move(options)));
};

auto tRewrittenArgv0 =
    test("ModuleInfo/standaloneWhenLaunchedWithRewrittenArgv0") = []
{
    checkModuleInfoStandalone(
        runModuleInfoHarnessWithArgv0("eacp-renamed-by-launcher"));
};

auto tRelativeElsewhere =
    test("ModuleInfo/standaloneWhenLaunchedByRelativePathFromElsewhere") = []
{
    const auto directory = harness.parent_path();
    const auto relative = directory.filename() / harness.filename();

    checkModuleInfoStandalone(
        runModuleInfoHarnessAs(relative.string(), directory.parent_path().string()));
};

auto tRelativeThenChdir =
    test("ModuleInfo/standaloneWhenRelativeLaunchChangesDirectory") = []
{
    checkModuleInfoStandalone(
        runModuleInfoHarnessAs("./" + harness.filename().string(),
                               harness.parent_path().string(),
                               {"--chdir", "/"}));
};
