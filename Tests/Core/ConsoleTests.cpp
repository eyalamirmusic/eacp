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

bool startsWithProgramName(const std::string& text)
{
    // "ConsoleHarness.exe" on Windows: argv[0]'s file name, as written.
    return text.starts_with("ConsoleHarness");
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
    check(result.output == "hello\n");
    check(result.errorOutput.empty());
};

auto tExceptionIsReported =
    test("Console/an exception prints the program and exits 1") = []
{
    auto result = runHarness({"--throw"});

    check(result.exitCode == 1);
    check(result.output.empty());
    check(startsWithProgramName(result.errorOutput));
    check(result.errorOutput.ends_with(": boom\n"));
};

auto tBadArgumentsExit2 =
    test("Console/bad arguments print the error and exit 2") = []
{
    auto result = runHarness({"--exit", "abc"});

    check(result.exitCode == eacp::Args::badArguments);
    check(result.output.empty());
    check(result.errorOutput
          == "ConsoleHarness: --exit needs a whole number, not 'abc'\n");
};

auto tHelpExits0 = test("Console/help prints to stdout and exits 0") = []
{
    auto result = runHarness({"--help"});

    check(result.exitCode == 0);
    check(result.output.starts_with("Usage: ConsoleHarness [options]\n"));
    check(result.output.find("--say <value>") != std::string::npos);
    check(result.errorOutput.empty());
};

Proc::ProcessResult runHarnessOnStoredArgs(const eacp::Vector<std::string>& args)
{
    auto options = Proc::ProcessOptions {};
    options.executable = EACP_CONSOLE_HARNESS;
    options.arguments = args;
    options.environment.add({"EACP_CONSOLE_HARNESS_STORED", "1"});
    return Proc::run(std::move(options));
}

auto tStoredArgs =
    test("Console/runConsole(body) runs on the stored command line") = []
{
    auto result = runHarnessOnStoredArgs({"--exit", "4", "--say", "stored"});

    check(result.exitCode == 4);
    check(result.output == "stored\n");
    check(result.errorOutput.empty());

    auto failed = runHarnessOnStoredArgs({"--throw"});

    check(failed.exitCode == 1);
    check(startsWithProgramName(failed.errorOutput));
};

auto tFailReportsLikeAParseError =
    test("Console/Parser::fail prints the error and exits 2") = []
{
    auto result = runHarness({"--fail", "nothing to validate"});

    check(result.exitCode == eacp::Args::badArguments);
    check(result.output.empty());
    check(result.errorOutput == "ConsoleHarness: nothing to validate\n");
};
} // namespace ConsoleTests
