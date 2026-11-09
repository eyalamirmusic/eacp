// The Windows half of the Process option tests, through cmd.exe: the verbatim
// commandLine (cmd's own `&&` and quoting must reach it untouched), stderr
// merged into stdout, the live output callback and the working directory.

#include "Common.h"

#include <mutex>

using namespace nano;
namespace Proc = eacp::Processes;

namespace
{
// cmd ends lines with CRLF and keeps the space before a `&&` or a redirection
// in the echoed text, so the cases compare trimmed lines.
eacp::Vector<std::string> linesOf(const std::string& text)
{
    auto lines = eacp::Vector<std::string> {};
    auto current = std::string {};

    auto finish = [&]
    {
        while (!current.empty() && current.back() == ' ')
            current.pop_back();

        if (!current.empty())
            lines.push_back(current);

        current.clear();
    };

    for (auto c: text)
    {
        if (c == '\r' || c == '\n')
            finish();
        else
            current.push_back(c);
    }

    finish();
    return lines;
}

Proc::ProcessOptions bothStreams()
{
    auto options = Proc::ProcessOptions {};
    options.commandLine = "cmd /c \"echo one&& echo two 1>&2&& echo three\"";
    return options;
}
} // namespace

auto tCommandLineChain = test("Process/commandLine/cmdChainIsVerbatim") = []
{
    auto options = Proc::ProcessOptions {};
    options.commandLine = "cmd /c \"echo a && echo b\"";

    auto result = Proc::run(std::move(options));

    check(result.launched);
    check(result.exitCode == 0);
    check(linesOf(result.output) == eacp::Vector<std::string> {"a", "b"});
};

auto tCommandLineQuotedArgument =
    test("Process/commandLine/quotedArgumentKeepsSpaces") = []
{
    auto options = Proc::ProcessOptions {};
    options.commandLine = "cmd /c \"if \"a b\"==\"a b\" echo same\"";

    auto result = Proc::run(std::move(options));

    check(result.exitCode == 0);
    check(linesOf(result.output) == eacp::Vector<std::string> {"same"});
};

auto tCommandLineReplacesExecutable =
    test("Process/commandLine/replacesExecutableAndArguments") = []
{
    auto options = Proc::ProcessOptions {"cmd", {"/c", "exit 3"}};
    options.commandLine = "cmd /c \"echo replaced\"";

    auto result = Proc::run(std::move(options));

    check(result.exitCode == 0);
    check(linesOf(result.output) == eacp::Vector<std::string> {"replaced"});
};

auto tCommandLineExitCode = test("Process/commandLine/exitCode") = []
{
    auto options = Proc::ProcessOptions {};
    options.commandLine = "cmd /c \"exit 7\"";

    auto result = Proc::run(std::move(options));

    check(result.launched);
    check(result.exitCode == 7);
};

auto tSeparateStreams = test("Process/run/separateStreams") = []
{
    auto result = Proc::run(bothStreams());

    check(result.exitCode == 0);
    check(linesOf(result.output) == eacp::Vector<std::string> {"one", "three"});
    check(linesOf(result.errorOutput) == eacp::Vector<std::string> {"two"});
};

auto tMergedStreams = test("Process/run/mergedStreams") = []
{
    auto options = bothStreams();
    options.mergeErrorOutput = true;

    auto result = Proc::run(std::move(options));

    check(result.exitCode == 0);
    check(linesOf(result.output)
          == eacp::Vector<std::string> {"one", "two", "three"});
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
    check(!seen.empty());
    check(seen == result.output);
};

auto tWorkingDirectory = test("Process/run/workingDirectory") = []
{
    auto dir = scratchDirectory("process-working-directory");

    auto options = Proc::ProcessOptions {"cmd", {"/c", "cd"}};
    options.workingDirectory = dir.string();

    auto result = Proc::run(std::move(options));
    auto lines = linesOf(result.output);

    check(result.exitCode == 0);
    check(lines.size() == 1);

    if (lines.size() == 1)
        check(std::filesystem::canonical(lines.front())
              == std::filesystem::canonical(dir));

    std::filesystem::remove_all(dir);
};
