#include "Common.h"

#include <eacp/Core/Utils/Environment.h>

#include <atomic>
#include <thread>

using namespace nano;
using namespace eacp;

namespace
{
constexpr auto variablesWrittenPerRound = 64;
constexpr auto writeRounds = 5000;
constexpr auto variableNamePrefix = "EACP_ENVIRONMENT_RACE_";
constexpr auto unsetVariableName = "EACP_ENVIRONMENT_RACE_NEVER_SET";

std::string getRaceVariableName(int index)
{
    return variableNamePrefix + std::to_string(index);
}
} // namespace

// Adding variables grows environ, which setenv may reallocate. Looking up a
// name that is never set walks the whole array, so a reader on another thread
// would touch the freed one if access were not serialised.
auto tEnvironmentReadsWhileAnotherThreadWrites =
    test("Environment/readsWhileAnotherThreadWrites") = []
{
    auto writerFinished = std::atomic {false};
    auto reads = 0;
    auto missingReads = 0;

    auto reader = std::thread {[&]
                               {
                                   while (!writerFinished.load())
                                   {
                                       ++reads;

                                       if (!getEnv(unsetVariableName).has_value())
                                           ++missingReads;
                                   }
                               }};

    for (auto round = 0; round < writeRounds; ++round)
    {
        for (auto index = 0; index < variablesWrittenPerRound; ++index)
            setEnv(getRaceVariableName(index), "1");

        for (auto index = 0; index < variablesWrittenPerRound; ++index)
            unsetEnv(getRaceVariableName(index));
    }

    writerFinished = true;
    reader.join();

    check(missingReads == reads);
};

// The parent hands the child a non-ASCII value through the process environment;
// the child reads it with getEnvValue (EACP_CONSOLE_HARNESS_ARGV0 becomes its
// program name) and prints it back in its error line.
auto tEnvironmentNonAsciiReachesAChild =
    test("Environment/a non-ASCII value reaches getEnv in a child as UTF-8") = []
{
    const auto text = std::u8string {u8"\u00dcn\u00efc\u00f8d\u00e9-\u65e5\u672c"};
    const auto name = std::string {text.begin(), text.end()};

    auto options = Processes::ProcessOptions {};
    options.executable = EACP_CONSOLE_HARNESS;
    options.arguments = {"--throw"};
    options.environment.add(
        {"EACP_CONSOLE_HARNESS_ARGV0", "some/dir/" + name + ".exe"});
    auto result = Processes::run(std::move(options));

    auto errorOutput = result.errorOutput;
    std::erase(errorOutput, '\r');

    check(result.exitCode == 1);
    check(errorOutput == name + ": boom\n");
};
