#include <eacp/Core/Plugins/ReloadableEntry.h>

namespace
{
long answer()
{
    return 3;
}
} // namespace

EACP_RELOADABLE(answer)
