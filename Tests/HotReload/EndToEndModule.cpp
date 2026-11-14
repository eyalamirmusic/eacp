#include "EndToEndABI.h"

#include <eacp/Core/Plugins/ReloadableEntry.h>
#include <eacp/Core/Utils/Strings.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <timeapi.h>

namespace
{
constexpr auto step = 1;

// winmm is linked by this module alone: the app exports nothing of it.
int one()
{
    auto fromEacp = (int) eacp::Strings::trim("  x  ").size();
    return timeGetTime() != 0 ? fromEacp : 0;
}

int answer()
{
    return hotReloadEndToEndBase() + one() * step;
}
} // namespace

EACP_RELOADABLE(answer)
