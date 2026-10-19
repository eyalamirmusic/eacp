#include <eacp/Core/Plugins/PluginExport.h>

#include "LayoutABI.h"

constexpr auto padding = 16.f;
constexpr auto gap = 12.f;
constexpr auto titleHeight = 36.f;
constexpr auto footerHeight = 22.f;
constexpr auto statusHeight = 22.f;
constexpr auto sidebarWidth = 180.f;
constexpr auto rowHeight = 32.f;
constexpr auto muteWidth = 120.f;

EACP_PLUGIN_EXPORT void demoLayout(float width, float height, LayoutTable* table)
{
    auto left = padding;
    auto top = padding;
    auto right = width - padding;
    auto bottom = height - padding - statusHeight - gap;

    addLayoutEntry(table, "title", left, top, right - left, titleHeight);
    top += titleHeight + gap;

    addLayoutEntry(
        table, "footer", left, bottom - footerHeight, right - left, footerHeight);
    bottom -= footerHeight + gap;

    addLayoutEntry(table, "sidebar", left, top, sidebarWidth, bottom - top);
    left += sidebarWidth + gap;

    addLayoutEntry(
        table, "gain", left, top, right - left - muteWidth - gap, rowHeight);
    addLayoutEntry(table, "mute", right - muteWidth, top, muteWidth, rowHeight);
    top += rowHeight + gap;

    addLayoutEntry(table, "notes", left, top, right - left, rowHeight);
}
