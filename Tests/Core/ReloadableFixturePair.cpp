#include "ReloadableTestObject.h"
#include <eacp/Core/Plugins/ReloadableEntry.h>

namespace
{
struct PairObject final : ReloadableTestObject
{
    ~PairObject() override { markDestroyed(); }

    int value() const override { return 7; }
};

int answer()
{
    return 7;
}

ReloadableTestObject* makeObject()
{
    return new PairObject;
}
} // namespace

EACP_RELOADABLE(answer, makeObject)
