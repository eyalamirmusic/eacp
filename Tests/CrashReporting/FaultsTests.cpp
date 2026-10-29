#include <eacp/CrashReporting/Faults/Faults.h>

#include <NanoTest/NanoTest.h>

#include <set>
#include <string>

using namespace nano;
using namespace eacp::CrashReporting;

namespace FaultsTests
{
auto tFaultNamesAreUnique = test("CrashFaults/faultNamesAreUnique") = []
{
    auto names = std::set<std::string_view> {};

    for (const auto& fault: Faults::listFaults())
        check(names.insert(fault.name).second);

    check(names.size() == Faults::listFaults().size());
};

auto tFindsEveryFault = test("CrashFaults/findsEveryFaultByName") = []
{
    for (const auto& fault: Faults::listFaults())
    {
        auto found = Faults::findFault(fault.name);
        check(found.has_value() && &found->get() == &fault);
    }

    check(!Faults::findFault("no-such-fault"));
};

auto tStallingFaults = test("CrashFaults/hangSpinAndDeadlockStallTheirThread") = []
{
    for (auto name: {"hang", "spin", "deadlock"})
        check(Faults::findFault(name)->get().stallsItsThread);

    check(!Faults::findFault("nullptr")->get().stallsItsThread);
};

auto tFastFailIsWindowsOnly =
    test("CrashFaults/fastFailIsAvailableOnWindowsOnly") = []
{
#if defined(_WIN32)
    check(Faults::findFault("fastfail")->get().isAvailable);
#else
    check(!Faults::findFault("fastfail")->get().isAvailable);
    check(!Faults::triggerFault("fastfail", Faults::ThreadChoice::main));
#endif
};

auto tUnknownFaultIsRefused = test("CrashFaults/unknownFaultIsRefused") = []
{
    check(!Faults::triggerFault("no-such-fault", Faults::ThreadChoice::main));
    check(!Faults::triggerFault("no-such-fault", Faults::ThreadChoice::background));
    check(!Faults::scheduleFault(
        "no-such-fault", Faults::ThreadChoice::main, eacp::Time::MS {0}));
};

auto tThreadChoiceNames = test("CrashFaults/threadChoiceNamesRoundTrip") = []
{
    for (auto choice: {Faults::ThreadChoice::main, Faults::ThreadChoice::background})
        check(Faults::findThreadChoice(Faults::getThreadChoiceName(choice))
              == choice);

    check(!Faults::findThreadChoice("sideways"));
};
} // namespace FaultsTests
