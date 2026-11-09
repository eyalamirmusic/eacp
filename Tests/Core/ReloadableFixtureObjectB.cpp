#include "ReloadableTestObject.h"
#include <eacp/Core/Plugins/ReloadableEntry.h>

namespace
{
struct ObjectB final : ReloadableTestObject
{
    ~ObjectB() override { markDestroyed(); }

    int value() const override { return 2; }
};

ReloadableTestObject* makeObject()
{
    return new ObjectB;
}
} // namespace

EACP_RELOADABLE(makeObject)
