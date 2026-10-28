#include <eacp/CrashReporting/Zoo/Zoo.h>

#include <NanoTest/NanoTest.h>

#include <set>
#include <string>

using namespace nano;
using namespace eacp::CrashReporting;

namespace ZooTests
{
auto tFaultNamesAreUnique = test("CrashZoo/faultNamesAreUnique") = []
{
    auto names = std::set<std::string_view> {};

    for (const auto& fault: Zoo::listFaults())
        check(names.insert(fault.name).second);

    check(names.size() == Zoo::listFaults().size());
};

auto tFindsEveryFault = test("CrashZoo/findsEveryFaultByName") = []
{
    for (const auto& fault: Zoo::listFaults())
    {
        auto found = Zoo::findFault(fault.name);
        check(found.has_value() && &found->get() == &fault);
    }

    check(!Zoo::findFault("no-such-fault"));
};

auto tStallingFaults = test("CrashZoo/hangSpinAndDeadlockStallTheirThread") = []
{
    for (auto name: {"hang", "spin", "deadlock"})
        check(Zoo::findFault(name)->get().stallsItsThread);

    check(!Zoo::findFault("nullptr")->get().stallsItsThread);
};

auto tFastFailIsWindowsOnly = test("CrashZoo/fastFailIsAvailableOnWindowsOnly") = []
{
#if defined(_WIN32)
    check(Zoo::findFault("fastfail")->get().isAvailable);
#else
    check(!Zoo::findFault("fastfail")->get().isAvailable);
    check(!Zoo::triggerFault("fastfail", Zoo::ThreadChoice::main));
#endif
};

auto tUnknownFaultIsRefused = test("CrashZoo/unknownFaultIsRefused") = []
{
    check(!Zoo::triggerFault("no-such-fault", Zoo::ThreadChoice::main));
    check(!Zoo::triggerFault("no-such-fault", Zoo::ThreadChoice::background));
    check(!Zoo::scheduleFault(
        "no-such-fault", Zoo::ThreadChoice::main, eacp::Time::MS {0}));
};

auto tThreadChoiceNames = test("CrashZoo/threadChoiceNamesRoundTrip") = []
{
    for (auto choice: {Zoo::ThreadChoice::main, Zoo::ThreadChoice::background})
        check(Zoo::findThreadChoice(Zoo::getThreadChoiceName(choice)) == choice);

    check(!Zoo::findThreadChoice("sideways"));
};
} // namespace ZooTests
