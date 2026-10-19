#include <eacp/Core/App/Args.h>
#include <eacp/Core/Core.h>
#include <eacp/Core/Utils/Environment.h>

#include <stdexcept>

// A command-line tool built the way a real one is, runConsole over an
// Args::Parser, for ConsoleTests to run as a child and read the exit code and
// the two streams of. The body reads the command line runConsole stored, so it
// captures nothing.
namespace
{
int runCase()
{
    auto exitCode = 0;
    auto shouldThrow = false;
    auto say = std::string {};
    auto failWith = std::string {};

    auto cli = eacp::Args::Parser {"ConsoleHarness", "Runs one case."};
    cli.option("--exit", exitCode, "the code to return");
    cli.flag("--throw", shouldThrow, "throw instead of returning");
    cli.option("--say", say, "a line to print to stdout");
    cli.option("--fail", failWith, "report this through Parser::fail");

    if (auto code = cli.exitCodeFor(cli.parse()))
        return *code;

    if (!failWith.empty())
        return cli.fail(failWith);

    if (shouldThrow)
        throw std::runtime_error {"boom"};

    if (!say.empty())
        eacp::Console::print(say);

    return exitCode;
}
} // namespace

// EACP_CONSOLE_HARNESS_STORED=1 takes the other entry point: the command line
// stored first, then runConsole(body) with no argc/argv.
// EACP_CONSOLE_HARNESS_ARGV0 stands in for argv[0], so the program name
// runConsole derives can be tested with names this binary does not have.
int main(int argc, char* argv[])
{
    auto argv0 = eacp::getEnvValue("EACP_CONSOLE_HARNESS_ARGV0");

    if (!argv0.empty())
        argv[0] = argv0.data();

    if (eacp::getEnvValue("EACP_CONSOLE_HARNESS_STORED") != "1")
        return eacp::Apps::runConsole(argc, argv, runCase);

    eacp::Apps::setCommandLineArgs(argc, argv);
    return eacp::Apps::runConsole(runCase);
}
