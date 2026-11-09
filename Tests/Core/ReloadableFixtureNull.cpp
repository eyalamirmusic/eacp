#include "ReloadableTestObject.h"
#include <eacp/Core/Plugins/ReloadableEntry.h>

namespace
{
ReloadableTestObject* makeNothing()
{
    return nullptr;
}
} // namespace

EACP_RELOADABLE(makeNothing)
