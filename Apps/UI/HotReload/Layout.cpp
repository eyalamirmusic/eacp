#include "LayoutABI.h"

using eacp::UI::Rect;

namespace
{
constexpr auto padding = 16.f;
constexpr auto gap = 12.f;
constexpr auto titleHeight = 36.f;
constexpr auto footerHeight = 22.f;
constexpr auto statusHeight = 22.f;
constexpr auto sidebarWidth = 180.f;
constexpr auto sidebarMinimumWindowWidth = 600.f;
constexpr auto rowHeight = 32.f;
constexpr auto muteWidth = 120.f;
} // namespace

EACP_PLUGIN_EXPORT void demoLayout(const LayoutTargets& targets, const Rect& bounds)
{
    auto area = bounds.inset(padding);
    area.removeFromBottom(statusHeight + gap);

    targets.title.setBounds(area.removeFromTop(titleHeight));
    area.removeFromTop(gap);

    targets.footer.setBounds(area.removeFromBottom(footerHeight));
    area.removeFromBottom(gap);

    auto showSidebar = bounds.w >= sidebarMinimumWindowWidth;
    targets.sidebar.setVisible(showSidebar);

    if (showSidebar)
    {
        targets.sidebar.setBounds(area.removeFromLeft(sidebarWidth));
        area.removeFromLeft(gap);
    }

    auto row = area.removeFromTop(rowHeight);
    targets.mute.setBounds(row.removeFromRight(muteWidth));
    row.removeFromRight(gap);
    targets.gain.setBounds(row);
    area.removeFromTop(gap);

    targets.notes.setBounds(area.removeFromTop(rowHeight));
}
