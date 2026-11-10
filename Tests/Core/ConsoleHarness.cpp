#include <eacp/Core/App/Args.h>
#include <eacp/Core/Core.h>
#include <eacp/Core/Utils/Environment.h>

#include <stdexcept>

// A command-line tool built the way a real one is, runConsole over an
// Args::Parser, for ConsoleTests to run as a child and read the exit code and
// the two streams of. By default the body reads the command line runConsole
// stored, so it captures nothing.
namespace
{
using Parse = std::function<eacp::Args::Result(eacp::Args::Parser&)>;

int runCase(const Parse& parse)
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

    if (auto code = cli.exitCodeFor(parse(cli)))
        return *code;

    if (!failWith.empty())
        return cli.fail(failWith);

    if (shouldThrow)
        throw std::runtime_error {"boom"};

    if (!say.empty())
        eacp::Console::print(say);

    return exitCode;
}

int runOnStoredArgs()
{
    return runCase([](eacp::Args::Parser& cli) { return cli.parse(); });
}

bool modeIsOn(const char* name)
{
    return eacp::getEnvValue(name) == "1";
}
} // namespace

// EACP_CONSOLE_HARNESS_STORED=1 takes the other entry point: the command line
// stored first, then runConsole(body) with no argc/argv.
// EACP_CONSOLE_HARNESS_EXPLICIT=1 stores nothing and parses main()'s argc/argv
// with Parser::parse(argc, argv).
// EACP_CONSOLE_HARNESS_DROP_LAST=1 prints argv[2] to stderr as main() got it,
// then stores every argument but the last, a list that is not the process's
// command line.
// EACP_CONSOLE_HARNESS_ARGV0 stands in for argv[0], so the program name
// runConsole derives can be tested with names this binary does not have.
int main(int argc, char* argv[])
{
    auto argv0 = eacp::getEnvValue("EACP_CONSOLE_HARNESS_ARGV0");

    if (!argv0.empty())
        argv[0] = argv0.data();

    if (modeIsOn("EACP_CONSOLE_HARNESS_EXPLICIT"))
        return eacp::Apps::runConsole(
            [&]
            {
                return runCase([&](eacp::Args::Parser& cli)
                               { return cli.parse(argc, argv); });
            });

    if (modeIsOn("EACP_CONSOLE_HARNESS_DROP_LAST") && argc > 2)
    {
        eacp::Console::printError(argv[2]);
        eacp::Apps::setCommandLineArgs(argc - 1, argv);
        return eacp::Apps::runConsole(runOnStoredArgs);
    }

    if (!modeIsOn("EACP_CONSOLE_HARNESS_STORED"))
        return eacp::Apps::runConsole(argc, argv, runOnStoredArgs);

    eacp::Apps::setCommandLineArgs(argc, argv);
    return eacp::Apps::runConsole(runOnStoredArgs);
}
