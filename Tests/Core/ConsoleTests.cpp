#include "Common.h"

#include <eacp/Core/App/Args.h>

using namespace nano;

namespace ConsoleTests
{
namespace Proc = eacp::Processes;

Proc::ProcessResult runHarness(const eacp::Vector<std::string>& args)
{
    return Proc::run(EACP_CONSOLE_HARNESS, args);
}

Proc::ProcessResult runHarnessWith(const eacp::Vector<std::string>& args,
                                   const std::string& name,
                                   const std::string& value)
{
    auto options = Proc::ProcessOptions {};
    options.executable = EACP_CONSOLE_HARNESS;
    options.arguments = args;
    options.environment.add({name, value});
    return Proc::run(std::move(options));
}

Proc::ProcessResult runHarnessOnStoredArgs(const eacp::Vector<std::string>& args)
{
    return runHarnessWith(args, "EACP_CONSOLE_HARNESS_STORED", "1");
}

Proc::ProcessResult runHarnessNamed(const std::string& argv0,
                                    const eacp::Vector<std::string>& args)
{
    return runHarnessWith(args, "EACP_CONSOLE_HARNESS_ARGV0", argv0);
}

// A Windows console program's text-mode streams write "\n" as "\r\n", which
// is correct there, so the captured text is compared with the '\r's removed.
std::string withLfEndings(std::string text)
{
    std::erase(text, '\r');
    return text;
}

auto tReturnsTheBodysCode =
    test("Console/runConsole returns what the body returns") = []
{
    auto result = runHarness({"--exit", "3"});

    check(result.launched);
    check(result.exited);
    check(result.exitCode == 3);
    check(result.output.empty());
    check(result.errorOutput.empty());
};

auto tPrintGoesToStdout = test("Console/print writes a line to stdout") = []
{
    auto result = runHarness({"--say", "hello"});

    check(result.exitCode == 0);
    check(withLfEndings(result.output) == "hello\n");
    check(result.errorOutput.empty());
};

auto tExceptionIsReported =
    test("Console/an exception prints the program and exits 1") = []
{
    auto result = runHarness({"--throw"});

    check(result.exitCode == 1);
    check(result.output.empty());
    check(withLfEndings(result.errorOutput) == "ConsoleHarness: boom\n");
};

auto tProgramNameDropsExe =
    test("Console/the program name is argv[0]'s file name less .exe") = []
{
    auto exe = runHarnessNamed("some/dir/Tool.EXE", {"--throw"});

    check(exe.exitCode == 1);
    check(withLfEndings(exe.errorOutput) == "Tool: boom\n");

    auto dotted = runHarnessNamed("some/dir/tool.v2", {"--throw"});

    check(dotted.exitCode == 1);
    check(withLfEndings(dotted.errorOutput) == "tool.v2: boom\n");
};

auto tBadArgumentsExit2 =
    test("Console/bad arguments print the error and exit 2") = []
{
    auto result = runHarness({"--exit", "abc"});

    check(result.exitCode == eacp::Args::badArguments);
    check(result.output.empty());
    check(withLfEndings(result.errorOutput)
          == "ConsoleHarness: --exit needs a whole number, not 'abc'\n");
};

auto tHelpExits0 = test("Console/help prints to stdout and exits 0") = []
{
    auto result = runHarness({"--help"});
    auto output = withLfEndings(result.output);

    check(result.exitCode == 0);
    check(output.starts_with("Usage: ConsoleHarness [options]\n"));
    check(output.find("--say <value>") != std::string::npos);
    check(result.errorOutput.empty());
};

auto tStoredArgs =
    test("Console/runConsole(body) runs on the stored command line") = []
{
    auto result = runHarnessOnStoredArgs({"--exit", "4", "--say", "stored"});

    check(result.exitCode == 4);
    check(withLfEndings(result.output) == "stored\n");
    check(result.errorOutput.empty());

    auto failed = runHarnessOnStoredArgs({"--throw"});

    check(failed.exitCode == 1);
    check(withLfEndings(failed.errorOutput) == "ConsoleHarness: boom\n");
};

auto tFailReportsLikeAParseError =
    test("Console/Parser::fail prints the error and exits 2") = []
{
    auto result = runHarness({"--fail", "nothing to validate"});

    check(result.exitCode == eacp::Args::badArguments);
    check(result.output.empty());
    check(withLfEndings(result.errorOutput)
          == "ConsoleHarness: nothing to validate\n");
};
} // namespace ConsoleTests
