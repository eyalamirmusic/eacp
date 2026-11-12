#include "Common.h"

#include <filesystem>
#include <string>
#include <unistd.h>

using namespace nano;
namespace Proc = eacp::Processes;
namespace fs = std::filesystem;

namespace
{
const auto harness = fs::path {EACP_MODULE_INFO_HARNESS};

std::string classifyWhenLaunched(Proc::ProcessOptions options)
{
    const auto result = Proc::run(std::move(options));

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);

    return result.output;
}

std::string classifyWhenLaunchedAs(const std::string& executable,
                                   const std::string& workingDirectory = {})
{
    auto options = Proc::ProcessOptions {};
    options.executable = executable;
    options.workingDirectory = workingDirectory;

    return classifyWhenLaunched(std::move(options));
}
} // namespace

// Four spellings of one binary. A standalone executable is standalone whichever
// was typed. Linux used to answer "plugin" for the relative and bare-name ones,
// so run<T>() returned without starting the event loop and the app exited at
// once with status 0.
auto tAbsolutePath = test("ModuleInfo/standaloneWhenLaunchedByAbsolutePath") = []
{ check(classifyWhenLaunchedAs(harness.string()) == "standalone\n"); };

auto tRelativePath = test("ModuleInfo/standaloneWhenLaunchedByRelativePath") = []
{
    check(classifyWhenLaunchedAs("./" + harness.filename().string(),
                                 harness.parent_path().string())
          == "standalone\n");
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

    check(classifyWhenLaunchedAs(link.string()) == "standalone\n");

    fs::remove(link, ec);
};

auto tBareName = test("ModuleInfo/standaloneWhenLaunchedByBareNameFromPath") = []
{
    auto options = Proc::ProcessOptions {};
    options.executable = "/bin/sh";
    options.arguments = {"-c", harness.filename().string()};
    options.environment = {{"PATH", harness.parent_path().string()}};

    check(classifyWhenLaunched(std::move(options)) == "standalone\n");
};
