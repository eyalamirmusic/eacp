#include "Common.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace nano;

namespace ThreadTests
{
using eacp::Threads::Thread;

void pause()
{
    std::this_thread::sleep_for(std::chrono::milliseconds {20});
}

auto tDefaultIsEmpty = test("Thread/defaultHoldsNoThread") = []
{
    auto thread = Thread {};
    check(!thread.isJoinable());
    check(!thread.isCurrentThread());
    thread.join();
    check(!thread.isJoinable());
};

auto tRunsOnAnotherThread = test("Thread/bodyRunsOnAnotherThread") = []
{
    auto onMain = true;
    auto current = false;
    auto ran = false;
    auto thread = Thread {};

    auto body = [&]
    {
        onMain = eacp::Threads::isMainThread();
        current = thread.isCurrentThread();
        ran = true;
    };

    thread.start(body);
    check(thread.isJoinable());
    check(!thread.isCurrentThread());
    thread.join();

    check(ran);
    check(!onMain);
    check(current);
    check(!thread.isJoinable());
};

auto tConstructorStarts = test("Thread/constructorStartsTheBody") = []
{
    auto ran = std::atomic<bool> {false};
    auto body = [&] { ran = true; };

    auto thread = Thread {body};
    thread.join();

    check(ran);
};

auto tDestructionJoins = test("Thread/destructionJoins") = []
{
    auto finished = std::atomic<bool> {false};

    auto body = [&]
    {
        pause();
        finished = true;
    };

    {
        auto thread = Thread {body};
    }

    check(finished);
};

auto tStartAgain = test("Thread/startAfterARunRunsAgain") = []
{
    auto runs = std::atomic<int> {0};
    auto body = [&] { ++runs; };

    auto thread = Thread {body};
    thread.join();
    thread.start(body);
    thread.start(body);
    thread.join();

    check(runs == 3);
};

auto tMoveConstruction = test("Thread/moveConstructionTakesTheThread") = []
{
    auto finished = std::atomic<bool> {false};

    auto body = [&]
    {
        pause();
        finished = true;
    };

    auto source = Thread {body};
    auto target = Thread {std::move(source)};

    check(!source.isJoinable());
    check(target.isJoinable());

    target.join();
    check(finished);
};

auto tMoveAssignment = test("Thread/moveAssignmentJoinsAndTakesTheThread") = []
{
    auto first = std::atomic<bool> {false};
    auto second = std::atomic<bool> {false};

    auto firstBody = [&]
    {
        pause();
        first = true;
    };

    auto secondBody = [&]
    {
        pause();
        second = true;
    };

    auto target = Thread {firstBody};
    auto source = Thread {secondBody};

    target = std::move(source);

    check(first);
    check(!source.isJoinable());
    check(target.isJoinable());

    target.join();
    check(second);
};

auto tJoinTwice = test("Thread/joinTwiceIsHarmless") = []
{
    auto ran = std::atomic<bool> {false};
    auto body = [&] { ran = true; };

    auto thread = Thread {body};
    thread.join();
    thread.join();

    check(ran);
    check(!thread.isJoinable());
};
} // namespace ThreadTests
