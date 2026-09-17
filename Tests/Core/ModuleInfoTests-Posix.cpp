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

std::string classifyWhenLaunchedAs(const std::string& executable,
                                   const std::string& workingDirectory = {})
{
    auto options = Proc::ProcessOptions {};
    options.executable = executable;
    options.workingDirectory = workingDirectory;

    const auto result = Proc::run(std::move(options));

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 0);

    return result.output;
}
} // namespace

// Three spellings of one binary. A standalone executable is standalone whichever
// was typed, and the relative one is what Linux used to answer "plugin" for:
// /proc/self/exe is resolved and the path dladdr reports is not, so `./harness`
// compared unequal to itself. run<T>() then handed the event loop to a host that
// was not there and returned, so the process exited at once with status 0.
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
