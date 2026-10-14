#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// A pointer that goes null with its component: what a deferred callback holds
// so it can tell whether the thing it was for is still there.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace SafePointerTests
{
namespace
{
struct Panel : Component
{
    int value = 42;
};

struct Watcher : Component
{
    explicit Watcher(SafePointer<Watcher>& toCheck)
        : checked(toCheck)
    {
    }

    ~Watcher() override { seenInDestructor = checked.get(); }

    SafePointer<Watcher>& checked;
    static inline Watcher* seenInDestructor = nullptr;
};
} // namespace

auto tSafePointerFollowsTheComponent =
    test("SafePointer/pointsAtALiveComponent") = []
{
    auto panel = Panel {};
    auto safe = SafePointer<Panel> {&panel};

    check(safe.get() == &panel);
    check(safe != nullptr);
    check(safe->value == 42);
    check((*safe).value == 42);

    Panel* raw = safe;
    check(raw == &panel);
};

auto tSafePointerGoesNull =
    test("SafePointer/goesNullWhenTheComponentIsDestroyed") = []
{
    auto panel = std::make_unique<Panel>();
    auto safe = SafePointer<Panel> {panel.get()};
    auto copy = safe;
    auto asBase = SafePointer<Component> {panel.get()};

    panel.reset();

    check(safe == nullptr);
    check(copy.get() == nullptr);
    check(asBase.get() == nullptr);
    check(!safe);
};

auto tSafePointerDefaultsToNull = test("SafePointer/defaultAndNullAreNull") = []
{
    auto empty = SafePointer<Panel> {};
    auto fromNull = SafePointer<Panel> {nullptr};

    check(empty == nullptr);
    check(fromNull.get() == nullptr);
};

auto tSafePointerReassigns = test("SafePointer/canBeReassigned") = []
{
    auto first = std::make_unique<Panel>();
    auto second = Panel {};

    auto safe = SafePointer<Panel> {first.get()};
    safe = &second;
    first.reset();

    check(safe.get() == &second, "the old target going away is no concern");

    safe = nullptr;
    check(safe == nullptr);
};

// Component's destructor is what clears it, so a derived destructor still sees
// the component -- the same order juce::Component::SafePointer has.
auto tSafePointerInDerivedDestructor =
    test("SafePointer/isStillSetWhileTheDerivedDestructorRuns") = []
{
    auto safe = SafePointer<Watcher> {};

    {
        auto watcher = Watcher {safe};
        safe = &watcher;
    }

    check(Watcher::seenInDestructor != nullptr);
    check(safe == nullptr);
};
} // namespace SafePointerTests
