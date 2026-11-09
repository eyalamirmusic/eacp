#include "../Common.h"
#include "ProcessChildModes.h"

#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace nano;
namespace Proc = eacp::Processes;

namespace
{
constexpr auto concurrentChildCount = 8;
constexpr auto startRounds = 10;
constexpr auto exitDeadline = std::chrono::seconds {5};

using ProcessList = std::vector<std::unique_ptr<Proc::Process>>;

ProcessList startChildrenConcurrently()
{
    auto children = ProcessList(concurrentChildCount);
    auto starters = std::vector<std::thread> {};

    for (auto& child: children)
        starters.emplace_back(
            [&child]
            {
                child = std::make_unique<Proc::Process>(
                    ProcessChildModes::createChildOptions(
                        ProcessChildModes::echoInput));
            });

    for (auto& starter: starters)
        starter.join();

    return children;
}

// A sibling holding a leaked pipe end keeps wait() blocked, so a failing round
// kills every child to let it return.
void killAll(ProcessList& children)
{
    for (auto& child: children)
        child->kill();
}
} // namespace

// wait() returns once a child has exited and its stdout and stderr have closed,
// which a sibling started beside it holds up if it inherited any of its pipes.
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

            auto finished =
                std::async(std::launch::async, [&child] { child->wait(); });
            const auto inTime =
                finished.wait_for(exitDeadline) == std::future_status::ready;
            check(inTime);

            if (!inTime)
            {
                killAll(children);
                return;
            }
        }
    }
};
