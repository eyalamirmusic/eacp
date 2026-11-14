#include "Common.h"

#include <eacp/Core/Utils/TemporaryFile.h>

using namespace nano;
using namespace ExportListTesting;

namespace ExportListToolTests
{
namespace Proc = eacp::Processes;

Proc::ProcessResult runTool(const Vector<std::string>& args)
{
    return Proc::run(EXPORT_LIST_TOOL, args);
}

struct ToolRun
{
    TemporaryFile directory {};
    FilePath output = directory.path() / "App.def";
    FilePath inputs = directory.path() / "inputs.txt";

    ToolRun() { Files::createDirectories(directory.path()); }

    Proc::ProcessResult run(const Vector<std::string>& inputFiles,
                            const Vector<std::string>& extra = {}) const
    {
        auto list = std::string {};

        for (const auto& file: inputFiles)
            list += "\"" + file + "\"\n\n";

        Files::writeFile(inputs, list);

        auto args = Vector<std::string> {
            "--output", output.str(), "--name", "App.exe", "--inputs", inputs.str()};
        args.addFrom(extra);
        return runTool(args);
    }
};

auto tWritesThenLeavesAlone =
    test("ExportList/tool/writesOnceThenLeavesTheListAlone") = []
{
    auto run = ToolRun {};
    auto first = run.run({EXPORT_LIST_CLASSIC, EXPORT_LIST_ARCHIVE});
    check(first.exitCode == 0, first.errorOutput);
    check(first.output.find("written") != std::string::npos, first.output);

    auto text = Files::readFile(run.output);
    check(text.starts_with("NAME App.exe\nEXPORTS\n"));
    check(text.find(Symbols::plainFunction) != std::string::npos);

    auto second = run.run({EXPORT_LIST_CLASSIC, EXPORT_LIST_ARCHIVE});
    check(second.exitCode == 0, second.errorOutput);
    check(second.output.find("unchanged") != std::string::npos, second.output);
    check(Files::readFile(run.output) == text);
};

auto tUnfilteredIsLarger = test("ExportList/tool/unfilteredListsMore") = []
{
    auto filtered = ToolRun {};
    auto unfiltered = ToolRun {};
    check(filtered.run({EXPORT_LIST_CLASSIC}).exitCode == 0);
    check(unfiltered.run({EXPORT_LIST_CLASSIC}, {"--unfiltered"}).exitCode == 0);
    check(Files::readFile(unfiltered.output).size()
          > Files::readFile(filtered.output).size());
};

auto tRefusalRemovesStaleList =
    test("ExportList/tool/refusalRemovesTheStaleList") = []
{
    auto run = ToolRun {};
    check(run.run({EXPORT_LIST_CLASSIC}).exitCode == 0);
    check(Files::isFile(run.output));

    auto refused = run.run({EXPORT_LIST_CLASSIC, EXPORT_LIST_LINK_TIME_CODE});
    check(refused.exitCode == 1);
    check(
        mentionsAll(refused.errorOutput, {"LinkTimeCode", "Link-time optimisation"}),
        refused.errorOutput);
    check(!Files::isFile(run.output));
};

auto tFailsAboveLimit = test("ExportList/tool/failsAboveTheLimitAndWarnsNearIt") = []
{
    auto run = ToolRun {};
    auto over = run.run({EXPORT_LIST_CLASSIC}, {"--limit", "3"});
    check(over.exitCode == 1);
    check(mentionsAll(over.errorOutput, {"App.exe", "more than the 3"}),
          over.errorOutput);
    check(!Files::isFile(run.output));

    auto near = run.run({EXPORT_LIST_CLASSIC}, {"--warn-at", "3"});
    check(near.exitCode == 0, near.errorOutput);
    check(near.errorOutput.find("warning") != std::string::npos, near.errorOutput);
};

// The list eacp_add_export_list wrote at build time is what the host was
// linked with, and the module, linked against nothing but the import library
// made from it, finds every function it calls in the host.
auto tHostRunsModule =
    test("ExportList/cmake/hostRunsAModuleLinkedAgainstTheList") = []
{
    auto text = Files::readFile(EXPORT_LIST_FIXTURE_DEF);
    check(text.starts_with("NAME ExportListFixtureHost.exe\nEXPORTS\n"));
    check(text.find(Symbols::archiveOnly) != std::string::npos);

    auto result = Proc::run(EXPORT_LIST_FIXTURE_HOST, {EXPORT_LIST_FIXTURE_MODULE});
    check(result.exitCode == 0, result.output);
    check(result.output.find("exportListModuleEntry(2) = 20") != std::string::npos,
          result.output);
};

auto tRefusesNoInputs = test("ExportList/tool/refusesAnEmptyInputList") = []
{
    auto run = ToolRun {};
    auto result = run.run({});
    check(result.exitCode == 1);
    check(result.errorOutput.find("no inputs") != std::string::npos,
          result.errorOutput);
};
} // namespace ExportListToolTests
