#include "Common.h"

#include <memory>
#include <thread>

using namespace nano;
namespace Proc = eacp::Processes;

namespace
{
constexpr auto concurrentChildCount = 8;
constexpr auto startRounds = 10;
constexpr auto exitDeadline = eacp::Time::MS {5000};
constexpr auto exitPollInterval = 10;

// Reads stdin until end-of-file and prints nothing until then.
std::string getStdinReadingCommandLine()
{
    if constexpr (eacp::Platform::isWindows())
        return "sort";
    else
        return "cat";
}

using ProcessList = std::vector<std::unique_ptr<Proc::Process>>;

ProcessList startChildrenConcurrently()
{
    auto children = ProcessList(concurrentChildCount);
    auto starters = std::vector<std::thread> {};

    for (auto& child: children)
        starters.emplace_back(
            [&child]
            {
                auto options = Proc::ProcessOptions {};
                options.commandLine = getStdinReadingCommandLine();
                child = std::make_unique<Proc::Process>(std::move(options));
            });

    for (auto& starter: starters)
        starter.join();

    return children;
}

bool exitsBeforeDeadline(Proc::Process& process)
{
    const auto deadline = eacp::Time::Deadline {exitDeadline};

    while (process.isRunning())
    {
        if (deadline.expired())
            return false;

        eacp::Time::sleepMS(exitPollInterval);
    }

    return true;
}

// A sibling holding a leaked stdout end would keep a reader joined forever in
// ~Process, so a failing round kills every child before destroying any.
void killAll(ProcessList& children)
{
    for (auto& child: children)
        child->kill();
}
} // namespace

// Each child must see end-of-file as soon as its own stdin closes, which only
// holds when no sibling started beside it inherited that pipe's write end.
auto tConcurrentChildrenOwnTheirPipes =
    test("Process/concurrent/childSeesEndOfFileWhileSiblingsRun") = []
{
    for (auto round = 0; round < startRounds; ++round)
    {
        auto children = startChildrenConcurrently();

        for (auto& child: children)
            check(child->launched());

        for (auto& child: children)
        {
            child->closeInput();

            const auto exited = exitsBeforeDeadline(*child);
            check(exited);

            if (!exited)
            {
                killAll(children);
                return;
            }
        }
    }
};
