#include "Common.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace nano;
namespace Proc = eacp::Processes;
namespace fs = std::filesystem;

namespace
{
const auto harness = fs::path {EACP_MODULE_INFO_HARNESS};

struct HarnessReport
{
    std::string classification;
    std::string applicationPath;
};

HarnessReport runHarness(Proc::ProcessOptions options)
{
    const auto result = Proc::run(std::move(options));

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);

    auto report = HarnessReport {};
    auto lines = std::istringstream {result.output};
    std::getline(lines, report.classification);
    std::getline(lines, report.applicationPath);

    return report;
}

std::string classifyWhenLaunchedAs(const std::string& executable,
                                   const std::string& workingDirectory = {})
{
    auto options = Proc::ProcessOptions {};
    options.executable = executable;
    options.workingDirectory = workingDirectory;

    return runHarness(std::move(options)).classification;
}

// A shell finds the harness on a PATH holding only its directory, so the program
// receives its bare name as argv[0], as a command typed at a prompt does.
HarnessReport launchByBareNameThroughPath(const std::string& workingDirectory)
{
    auto options = Proc::ProcessOptions {};
    options.executable = "/bin/sh";
    options.arguments = {"-c", "exec " + harness.filename().string()};
    options.workingDirectory = workingDirectory;
    options.environment = {{"PATH", harness.parent_path().string()}};

    return runHarness(std::move(options));
}
} // namespace

// Three spellings of one binary. A standalone executable is standalone whichever
// was typed, and the relative one is what Linux used to answer "plugin" for:
// /proc/self/exe is resolved and the path dladdr reports is not, so `./harness`
// compared unequal to itself. run<T>() then handed the event loop to a host that
// was not there and returned, so the process exited at once with status 0.
auto tAbsolutePath = test("ModuleInfo/standaloneWhenLaunchedByAbsolutePath") = []
{ check(classifyWhenLaunchedAs(harness.string()) == "standalone"); };

auto tRelativePath = test("ModuleInfo/standaloneWhenLaunchedByRelativePath") = []
{
    check(classifyWhenLaunchedAs("./" + harness.filename().string(),
                                 harness.parent_path().string())
          == "standalone");
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

    check(classifyWhenLaunchedAs(link.string()) == "standalone");

    fs::remove(link, ec);
};

// On Linux the loader names a program found on PATH by its bare name, which
// eacp used to resolve against the working directory: any directory but the
// binary's own made the program a plugin and its application path wrong.
auto tBareName = test("ModuleInfo/standaloneWhenLaunchedByBareNameThroughPath") = []
{
    const auto report =
        launchByBareNameThroughPath(fs::temp_directory_path().string());

    check(report.classification == "standalone");
    check(report.applicationPath == fs::weakly_canonical(harness).string());
};
