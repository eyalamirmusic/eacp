#include <eacp/Core/Plugins/ReloadableEntry.h>

namespace
{
int answer()
{
    return 2;
}
} // namespace

EACP_RELOADABLE(answer)
