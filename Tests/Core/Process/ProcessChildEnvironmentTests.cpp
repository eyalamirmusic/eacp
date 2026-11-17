#include "../Common.h"
#include "ProcessChildModes.h"
#include "StdioCapture.h"

#include <filesystem>

using namespace nano;
namespace Proc = eacp::Processes;
using StdioCapture::contains;
using StdioCapture::contentsOf;
using StdioCapture::tempPath;

namespace
{
// PATH is set in any environment the tests run in, on every platform, so the
// cases remove it rather than set a variable of their own first.
constexpr auto alwaysSetName = "PATH";
constexpr auto overrideValue = "override";

Proc::ProcessResult runWithStderrToFile(Proc::ProcessOptions options,
                                        const std::filesystem::path& file)
{
    auto redirect = StdioCapture::StderrToFile {file};
    return Proc::run(std::move(options));
}
} // namespace

auto tRemovedThenOverridden = test("Process/removedEnvironment/overrideWins") = []
{
    auto options = ProcessChildModes::createChildOptions(
        ProcessChildModes::printEnvironment, {alwaysSetName});
    options.removedEnvironment = {alwaysSetName};
    options.environment = {{alwaysSetName, overrideValue}};

    auto result = Proc::run(std::move(options));

    check(result.output == overrideValue);
};

auto tMakeChildEnvironment = test("Process/makeChildEnvironment") = []
{
    constexpr auto childOnlyName = "EACP_CHILD_ONLY";
    constexpr auto childOnlyEntry = "EACP_CHILD_ONLY=1";

    auto entries =
        Proc::makeChildEnvironment({{childOnlyName, "1"}}, {alwaysSetName});

    const auto prefix = std::string {alwaysSetName} + "=";
    check(entries.findIf([&](const std::string& entry)
                         { return entry.starts_with(prefix); })
          == nullptr);
    check(entries.contains(std::string {childOnlyEntry}));
};

// The child writes "one" and "three" to stdout and "two" to stderr.
auto tInheritErrorOutput =
    test("Process/inheritErrorOutput/reachesLauncherStderr") = []
{
    const auto file = tempPath("eacp-inherit-error.txt");
    auto options =
        ProcessChildModes::createChildOptions(ProcessChildModes::writeBothStreams);
    options.inheritErrorOutput = true;

    auto result = runWithStderrToFile(std::move(options), file);
    auto launcherError = contentsOf(file);
    std::filesystem::remove(file);

    check(result.exitCode == 0);
    check(contains(result.output, "one"));
    check(contains(result.output, "three"));
    check(!contains(result.output, "two"));
    check(result.errorOutput.empty());
    check(contains(launcherError, "two"));
};

auto tMergeWinsOverInherit = test("Process/inheritErrorOutput/mergeWins") = []
{
    const auto file = tempPath("eacp-inherit-error-merged.txt");
    auto options =
        ProcessChildModes::createChildOptions(ProcessChildModes::writeBothStreams);
    options.inheritErrorOutput = true;
    options.mergeErrorOutput = true;

    auto result = runWithStderrToFile(std::move(options), file);
    auto launcherError = contentsOf(file);
    std::filesystem::remove(file);

    check(contains(result.output, "two"));
    check(result.errorOutput.empty());
    check(launcherError.empty());
};
