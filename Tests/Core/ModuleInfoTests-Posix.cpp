#include "Common.h"

#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <sstream>
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
    auto lines = std::istringstream {output};

    std::getline(lines, report.role);
    std::getline(lines, report.application);

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

// Processes::run always passes the executable as argv[0].
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

// Only Linux ever derived the path from argv[0]; macOS takes it from dyld.
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

// One binary launched every way. Linux once compared /proc/self/exe with
// argv[0] and took several of these for a plugin.
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
