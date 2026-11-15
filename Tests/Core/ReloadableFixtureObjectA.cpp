#include "ReloadableTestObject.h"
#include <eacp/Core/Plugins/ReloadableEntry.h>

namespace
{
struct ObjectA final : ReloadableTestObject
{
    ~ObjectA() override { markDestroyed(); }

    int value() const override { return 1; }
};

ReloadableTestObject* makeObject()
{
    return new ObjectA;
}
} // namespace

EACP_RELOADABLE(makeObject)
