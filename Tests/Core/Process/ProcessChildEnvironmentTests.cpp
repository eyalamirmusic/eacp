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

bool hasEntryNamed(const eacp::Vector<std::string>& entries, const std::string& name)
{
    for (const auto& entry: entries)
        if (entry.starts_with(name + "="))
            return true;

    return false;
}

bool hasEntry(const eacp::Vector<std::string>& entries, const std::string& wanted)
{
    for (const auto& entry: entries)
        if (entry == wanted)
            return true;

    return false;
}

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

    check(!hasEntryNamed(entries, alwaysSetName));
    check(hasEntry(entries, childOnlyEntry));
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
