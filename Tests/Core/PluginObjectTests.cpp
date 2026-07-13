#include "Common.h"
#include <eacp/Core/Plugins/PluginObject.h>

#include <stdexcept>

using namespace nano;
using namespace eacp;

namespace
{
struct Counted
{
    explicit Counted(int valueToUse)
        : value(valueToUse)
    {
        ++liveCount;
    }

    ~Counted() { --liveCount; }

    int value;
    static inline int liveCount = 0;
};

struct ThrowsOnConstruction
{
    ThrowsOnConstruction() { throw std::runtime_error("no"); }
};

struct ThrowsOnDestruction
{
    // The case the noexcept boundary exists for: the host calls stop from its
    // own teardown, so a throw escaping here would terminate the process.
    ~ThrowsOnDestruction() noexcept(false) { throw std::runtime_error("no"); }
};
} // namespace

auto tStartStop = test("PluginObject/startStop") = []
{
    check(Plugins::get<Counted>() == nullptr);

    check(Plugins::start<Counted>(7) == 0);
    check(Counted::liveCount == 1);
    check(Plugins::get<Counted>() != nullptr);
    check(Plugins::get<Counted>()->value == 7);

    Plugins::stop<Counted>();
    check(Counted::liveCount == 0);
    check(Plugins::get<Counted>() == nullptr);

    // Stopping what was never started, and stopping twice, are both no-ops.
    Plugins::stop<Counted>();
    check(Plugins::get<Counted>() == nullptr);
};

auto tStartReplaces = test("PluginObject/startReplaces") = []
{
    check(Plugins::start<Counted>(1) == 0);
    check(Plugins::start<Counted>(2) == 0);

    // The second start replaced the first — one live object, not two.
    check(Counted::liveCount == 1);
    check(Plugins::get<Counted>()->value == 2);

    Plugins::stop<Counted>();
    check(Counted::liveCount == 0);
};

auto tConstructorThrow = test("PluginObject/constructorThrowIsAReturnCode") = []
{
    // Reported to the host as a non-zero return, never as an exception across
    // the C boundary.
    check(Plugins::start<ThrowsOnConstruction>() != 0);
    check(Plugins::get<ThrowsOnConstruction>() == nullptr);
};

auto tDestructorThrow = test("PluginObject/destructorThrowIsSwallowed") = []
{
    check(Plugins::start<ThrowsOnDestruction>() == 0);

    // Would terminate the process if it escaped; stop() is noexcept.
    Plugins::stop<ThrowsOnDestruction>();
    check(Plugins::get<ThrowsOnDestruction>() == nullptr);
};
