#include <eacp/Core/Plugins/ReloadableEntry.h>

namespace
{
int answer()
{
    return 1;
}
} // namespace

EACP_RELOADABLE(answer)
