#include "OnlineResourceMonitor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace eacp::UI
{
namespace
{
using Entry = OnlineResources::Entry;
using Status = OnlineResources::Status;

constexpr auto padding = 12.f;
constexpr auto rowHeight = 48.f;
constexpr auto buttonWidth = 96.f;
constexpr auto buttonHeight = 28.f;
constexpr auto buttonGap = 8.f;
constexpr auto pollHz = 10;

constexpr auto goodColour = Color {0.45f, 0.85f, 0.55f, 1.f};
constexpr auto badColour = Color {1.f, 0.55f, 0.5f, 1.f};

std::string formatBytes(std::int64_t bytes)
{
    char buffer[32] = {};
    auto value = (double) bytes;

    if (bytes >= 1024 * 1024 * 1024)
        std::snprintf(
            buffer, sizeof(buffer), "%.2f GB", value / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024 * 1024)
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", value / (1024.0 * 1024.0));
    else
        std::snprintf(buffer, sizeof(buffer), "%.0f KB", value / 1024.0);

    return buffer;
}

std::string percentText(float fraction)
{
    return std::to_string((int) std::lround(std::clamp(fraction, 0.f, 1.f) * 100.f))
           + "%";
}

std::string statusText(const Entry& entry)
{
    using Stage = OnlineResource::Progress::Stage;

    switch (entry.status)
    {
        case Status::fetching:
            if (entry.progress.stage == Stage::extracting)
                return "unpacking";

            if (entry.progress.fraction >= 0.f)
                return "downloading " + percentText(entry.progress.fraction);

            return "downloading";

        case Status::failed:
            return "failed";

        case Status::cancelled:
            return entry.available ? "cancelled, copy kept" : "cancelled";

        default:
            break;
    }

    if (entry.available)
        return "on disk  " + formatBytes(entry.sizeOnDisk);

    return "not downloaded";
}

Color statusColour(const Entry& entry)
{
    const auto& theme = defaultTheme();

    switch (entry.status)
    {
        case Status::fetching:
            return theme.accent;
        case Status::failed:
            return badColour;
        default:
            break;
    }

    return entry.available ? goodColour : theme.dimText;
}

std::string detailText(const Entry& entry)
{
    if (entry.isFetching())
    {
        auto received = formatBytes(entry.progress.bytesReceived);

        if (entry.progress.totalBytes > 0)
            return received + " of " + formatBytes(entry.progress.totalBytes);

        return received + " so far";
    }

    if (entry.status == Status::failed)
        return entry.error;

    return entry.info.url;
}

bool anyFetching(const Vector<Entry>& rows)
{
    return std::any_of(rows.begin(),
                       rows.end(),
                       [](const Entry& entry) { return entry.isFetching(); });
}
} // namespace

OnlineResourceMonitor::OnlineResourceMonitor()
    : title("Online resources")
{
    title.setFontStyle(FontStyle::Bold);
    location.setColour(defaultTheme().dimText);
    location.setFontSize(11.f);
    summary.setColour(defaultTheme().dimText);
    summary.setJustification(Justification::Right);

    list.setModel(this);
    list.setRowHeight(rowHeight);

    fetchButton.onClick = [this] { fetchSelected(); };
    cancelButton.onClick = [this] { cancelSelected(); };
    removeButton.onClick = [this] { removeSelected(); };
    clearButton.onClick = [this] { clearAll(); };
    clearButton.setAccentColour(badColour);

    addChildren({title, location, summary, list, clearButton});
    addChildComponent(fetchButton);
    addChildComponent(cancelButton);
    addChildComponent(removeButton);

    listenerId = OnlineResources::get().addListener([this] { refresh(); });
    refresh();
}

OnlineResourceMonitor::~OnlineResourceMonitor()
{
    OnlineResources::get().removeListener(listenerId);
}

std::optional<OnlineResources::Entry> OnlineResourceMonitor::getSelectedEntry() const
{
    auto row = list.getSelectedRow();

    if (row < 0 || row >= rows.size())
        return std::nullopt;

    return rows[row];
}

void OnlineResourceMonitor::refresh()
{
    auto& registry = OnlineResources::get();
    rows = registry.entries();
    location.setText(registry.getDirectory().str());
    list.updateContent();
    updateSummary();
    updateButtons();
    updatePolling();
    finishPendingClear();
}

void OnlineResourceMonitor::fetchSelected()
{
    if (auto entry = getSelectedEntry(); entry && !entry->isFetching())
        OnlineResources::get().fetch(entry->path);
}

void OnlineResourceMonitor::cancelSelected()
{
    if (auto entry = getSelectedEntry(); entry && entry->isFetching())
        OnlineResources::get().cancel(entry->path);
}

void OnlineResourceMonitor::removeSelected()
{
    if (auto entry = getSelectedEntry(); entry && !entry->isFetching())
        OnlineResources::get().remove(entry->path);
}

// A transfer cannot be deleted from under itself, so the clear waits for
// the cancels it sends to land and finishes from the change they report.
void OnlineResourceMonitor::clearAll()
{
    auto& registry = OnlineResources::get();

    if (!anyFetching(rows))
    {
        registry.clear();
        return;
    }

    clearPending = true;

    for (const auto& entry: rows)
        if (entry.isFetching())
            registry.cancel(entry.path);
}

void OnlineResourceMonitor::finishPendingClear()
{
    if (!clearPending || anyFetching(rows))
        return;

    clearPending = false;
    OnlineResources::get().clear();
}

void OnlineResourceMonitor::updateButtons()
{
    auto entry = getSelectedEntry();
    auto fetching = entry && entry->isFetching();

    fetchButton.setVisible(entry && !fetching);
    cancelButton.setVisible(fetching);
    removeButton.setVisible(entry && !fetching && entry->available);
}

void OnlineResourceMonitor::updateSummary()
{
    auto onDisk = 0;
    auto fetching = 0;
    auto bytes = std::int64_t {0};

    for (const auto& entry: rows)
    {
        onDisk += entry.available ? 1 : 0;
        fetching += entry.isFetching() ? 1 : 0;
        bytes += entry.sizeOnDisk;
    }

    auto text =
        std::to_string(onDisk) + " of " + std::to_string(rows.size()) + " on disk";

    if (bytes > 0)
        text += "  " + formatBytes(bytes);

    if (fetching > 0)
        text += "  " + std::to_string(fetching) + " fetching";

    summary.setText(std::move(text));
}

void OnlineResourceMonitor::updatePolling()
{
    if (!anyFetching(rows))
    {
        poll.reset();
        return;
    }

    if (poll == nullptr)
        poll.create([this] { refresh(); }, pollHz);
}

void OnlineResourceMonitor::resized()
{
    auto area = getLocalBounds();
    area.x += padding;
    area.y += padding;
    area.w -= padding * 2.f;
    area.h -= padding * 2.f;

    title.setBounds({area.x, area.y, area.w, 20.f});
    location.setBounds({area.x, area.y + 20.f, area.w, 16.f});

    auto bar = Rect {area.x, area.bottom() - buttonHeight, area.w, buttonHeight};
    auto x = bar.x;

    for (auto* button: {&fetchButton, &cancelButton, &removeButton})
    {
        button->setBounds({x, bar.y, buttonWidth, buttonHeight});
        x += buttonWidth + buttonGap;
    }

    clearButton.setBounds(
        {bar.right() - buttonWidth, bar.y, buttonWidth, buttonHeight});
    summary.setBounds(
        {x, bar.y, std::max(0.f, clearButton.getBounds().x - x - buttonGap), bar.h});

    list.setBounds({area.x, area.y + 44.f, area.w, bar.y - area.y - 44.f - padding});
}

void OnlineResourceMonitor::paintRow(Graphics& g,
                                     int row,
                                     const Rect& bounds,
                                     bool selected)
{
    const auto& theme = defaultTheme();

    if (selected)
    {
        g.setColour(theme.accentDim);
        g.fillRect(bounds);
    }
    else if (row % 2 == 1)
    {
        g.setColour(theme.panel);
        g.fillRect(bounds);
    }

    if (row < 0 || row >= rows.size())
        return;

    const auto& entry = rows[row];
    auto left = bounds.x + 10.f;
    auto right = bounds.right() - 10.f;
    auto firstLine = bounds.y + 19.f;
    auto secondLine = bounds.y + 36.f;

    auto status = statusText(entry);
    auto statusWidth = g.measureText(status);

    g.setColour(theme.text);
    g.drawText(entry.info.name, {left, firstLine});

    g.setColour(statusColour(entry));
    g.drawText(status, {right - statusWidth, firstLine});

    g.setFontSize(11.f);
    g.setColour(entry.status == Status::failed ? badColour : theme.dimText);
    g.drawText(detailText(entry), {left, secondLine});

    if (!entry.isFetching())
        return;

    auto track = Rect {left, bounds.bottom() - 5.f, right - left, 3.f};
    g.setColour(theme.outline);
    g.fillRect(track);

    auto fraction = entry.progress.fraction;
    auto filled = track;
    filled.w = fraction >= 0.f ? track.w * std::clamp(fraction, 0.f, 1.f) : track.w;

    g.setColour(fraction >= 0.f ? theme.accent : theme.accentDim);
    g.fillRect(filled);
}

void OnlineResourceMonitor::selectedRowChanged(int)
{
    updateButtons();
}

void OnlineResourceMonitor::rowDoubleClicked(int row)
{
    if (row < 0 || row >= rows.size())
        return;

    if (rows[row].isFetching())
        OnlineResources::get().cancel(rows[row].path);
    else
        OnlineResources::get().fetch(rows[row].path);
}
} // namespace eacp::UI
