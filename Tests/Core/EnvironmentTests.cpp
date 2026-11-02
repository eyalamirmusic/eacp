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
