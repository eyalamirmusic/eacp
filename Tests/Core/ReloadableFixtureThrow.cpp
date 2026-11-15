#include "ReloadableTestObject.h"
#include <eacp/Core/Plugins/ReloadableEntry.h>

#include <stdexcept>

namespace
{
ReloadableTestObject* makeObject()
{
    throw std::runtime_error("no object today");
}
} // namespace

EACP_RELOADABLE(makeObject)
