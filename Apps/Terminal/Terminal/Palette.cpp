#include "Palette.h"

#include "FuzzyMatch.h"
#include "Projects.h"

#include <eacp/Graphics/Primitives/TextMetrics.h>

#include <algorithm>

namespace term
{
using namespace eacp;
using Graphics::Color;
using Graphics::Context;
using Graphics::KeyEvent;
using Graphics::MouseEvent;
using Graphics::Point;
using Graphics::Rect;
namespace KeyCode = Graphics::KeyCode;

namespace
{
constexpr float panelWidth = 680.0f;
constexpr float rowHeight = 34.0f;
constexpr float headerHeight = 46.0f;
constexpr int maxRows = 12;

std::string compactPath(std::string path)
{
    const auto home = FilePath::homeDirectory().str();

    if (!home.empty() && path.starts_with(home))
        path = "~" + path.substr(home.size());

    return path;
}

std::string truncated(const std::string& text, std::size_t max)
{
    if (text.size() <= max)
        return text;

    return text.substr(0, max - 1) + "…";
}
} // namespace

Palette::Palette(const AppConfig& configToUse, SessionManager& sessionsToUse)
    : config(configToUse)
    , sessions(sessionsToUse)
    , theme(themeByName(configToUse.theme))
    , queryFont({config.font, 16.0f})
    , rowFont({config.font, 14.0f})
    , detailFont({config.font, 12.0f})
{
    setHandlesMouseEvents(true);
    setGrabsFocusOnMouseDown(true);
}

void Palette::show()
{
    query.clear();
    selected = 0;
    shown = true;
    rebuild();
    applyQuery();
    repaint();
}

void Palette::rebuild()
{
    allItems.clear();

    for (auto& session: sessions.all())
    {
        auto item = PaletteItem {};
        item.kind = PaletteItem::Kind::Session;
        item.session = session.get();
        item.key = session->key();
        item.label = session->name;
        item.claude = session->isClaude();
        item.lastUsed = sessions.lastUsed(item.key);

        const auto& title = session->view.currentTitle();
        item.detail = item.claude && !title.empty()
                          ? title
                          : compactPath(session->view.currentCwd());

        if (!session->lastNotify.empty())
            item.status = session->lastNotify;

        allItems.push_back(std::move(item));
    }

    for (auto& project: scanProjects(config))
    {
        if (sessions.find(project.path) != nullptr)
            continue;

        auto item = PaletteItem {};
        item.kind = PaletteItem::Kind::Project;
        item.key = project.path;
        item.label = project.name;
        item.detail = compactPath(project.path);
        item.lastUsed = sessions.lastUsed(project.path);
        allItems.push_back(std::move(item));
    }
}

void Palette::applyQuery()
{
    visible.clear();

    if (query.empty())
    {
        // Open sessions first, then projects, each most-recently-used first.
        visible = allItems;
        std::stable_sort(visible.begin(),
                         visible.end(),
                         [](const auto& a, const auto& b)
                         {
                             if (a.kind != b.kind)
                                 return a.kind == PaletteItem::Kind::Session;

                             return a.lastUsed > b.lastUsed;
                         });
    }
    else
    {
        auto scored = std::vector<std::pair<int, const PaletteItem*>> {};

        for (const auto& item: allItems)
        {
            const auto haystack = item.label + " " + item.detail + " "
                                  + item.status
                                  + (item.claude ? " claude" : "");

            if (auto score = fuzzyScore(query, haystack))
                scored.emplace_back(*score, &item);
        }

        std::stable_sort(scored.begin(),
                         scored.end(),
                         [](const auto& a, const auto& b)
                         {
                             if (a.first != b.first)
                                 return a.first > b.first;

                             return a.second->lastUsed > b.second->lastUsed;
                         });

        for (auto& [score, item]: scored)
            visible.push_back(*item);
    }

    selected = std::clamp(selected, 0, std::max((int) visible.size() - 1, 0));
}

void Palette::choose()
{
    if (selected < (int) visible.size())
    {
        const auto item = visible[(std::size_t) selected];

        if (item.kind == PaletteItem::Kind::Session && item.session != nullptr)
            sessions.switchTo(*item.session);
        else
            sessions.openProject(item.key);
    }

    shown = false;
    onClosed();
}

void Palette::moveSelection(int delta)
{
    if (!visible.empty())
    {
        selected = (selected + delta + (int) visible.size())
                   % (int) visible.size();
        repaint();
    }
}

void Palette::popQueryChar()
{
    while (!query.empty())
    {
        const auto last = (unsigned char) query.back();
        query.pop_back();

        if ((last & 0xc0) != 0x80)
            break;
    }
}

void Palette::keyDown(const KeyEvent& event)
{
    if (event.keyCode == KeyCode::Escape
        || (event.modifiers.command
            && event.charactersIgnoringModifiers == "k"))
    {
        shown = false;
        onClosed();
        return;
    }

    if (event.keyCode == KeyCode::Return)
    {
        choose();
        return;
    }

    if (event.keyCode == KeyCode::UpArrow
        || (event.modifiers.control
            && event.charactersIgnoringModifiers == "p"))
    {
        moveSelection(-1);
        return;
    }

    if (event.keyCode == KeyCode::DownArrow
        || (event.modifiers.control
            && event.charactersIgnoringModifiers == "n"))
    {
        moveSelection(1);
        return;
    }

    if (event.keyCode == KeyCode::Delete)
    {
        popQueryChar();
        applyQuery();
        repaint();
        return;
    }

    if (event.modifiers.command || event.modifiers.control)
        return;

    const auto& text = event.characters;

    if (!text.empty() && (unsigned char) text[0] >= 0x20 && text[0] != 0x7f)
    {
        query += text;
        selected = 0;
        applyQuery();
        repaint();
    }
}

Rect Palette::panelBounds() const
{
    const auto bounds = getLocalBounds();
    const auto width = std::min(panelWidth, bounds.w - 60.0f);
    const auto rows = std::min((int) visible.size(), maxRows);
    const auto height = headerHeight + (float) std::max(rows, 1) * rowHeight
                        + 12.0f;

    return {(bounds.w - width) / 2.0f,
            std::max(bounds.h * 0.14f, 20.0f),
            width,
            height};
}

int Palette::rowAt(Point pos) const
{
    const auto panel = panelBounds();
    const auto y = pos.y - (panel.y + headerHeight + 4.0f);

    if (pos.x < panel.x || pos.x > panel.right() || y < 0)
        return -1;

    const auto row = (int) (y / rowHeight);
    return row < std::min((int) visible.size(), maxRows) ? row : -1;
}

void Palette::mouseMoved(const MouseEvent& event)
{
    if (const auto row = rowAt(event.pos); row >= 0 && row != selected)
    {
        selected = row;
        repaint();
    }
}

void Palette::mouseDown(const MouseEvent& event)
{
    const auto row = rowAt(event.pos);

    if (row >= 0)
    {
        selected = row;
        choose();
        return;
    }

    if (!panelBounds().contains(event.pos))
    {
        shown = false;
        onClosed();
    }
}

void Palette::paint(Context& context)
{
    const auto panel = panelBounds();

    context.setColor(Color::black(0.38f));
    context.fillRect(getLocalBounds());

    context.setColor(toColor(theme.background).brighter(0.04f));
    context.fillRoundedRect(panel, 12.0f);

    context.setColor(toColor(theme.selection, 0.8f));
    context.setLineWidth(1.0f);
    context.strokeRect(panel);

    // Query line
    const auto queryText = "› " + query + "▏";
    context.setColor(toColor(theme.foreground));
    context.drawText(queryText,
                     {panel.x + 18.0f, panel.y + 30.0f},
                     queryFont);

    context.setColor(toColor(theme.selection));
    context.drawLine({panel.x + 12.0f, panel.y + headerHeight - 2.0f},
                     {panel.right() - 12.0f, panel.y + headerHeight - 2.0f});

    // Rows
    const auto rows = std::min((int) visible.size(), maxRows);

    for (auto i = 0; i < rows; ++i)
    {
        const auto& item = visible[(std::size_t) i];
        const auto y = panel.y + headerHeight + 4.0f + (float) i * rowHeight;
        const auto rowRect =
            Rect {panel.x + 8.0f, y, panel.w - 16.0f, rowHeight - 2.0f};

        if (i == selected)
        {
            context.setColor(toColor(theme.selection, 0.85f));
            context.fillRoundedRect(rowRect, 6.0f);
        }

        const auto baseline = y + rowHeight * 0.62f;
        auto x = rowRect.x + 12.0f;

        const auto open = item.kind == PaletteItem::Kind::Session;
        const auto icon = item.claude ? "✳" : (open ? "●" : "▸");
        const auto iconColor = item.claude
                                   ? toColor(theme.ansi[5])
                                   : (open ? toColor(theme.ansi[2])
                                           : toColor(theme.ansi[8]));
        context.setColor(iconColor);
        context.drawText(icon, {x, baseline}, rowFont);
        x += 24.0f;

        context.setColor(toColor(theme.foreground));
        context.drawText(truncated(item.label, 32), {x, baseline}, rowFont);
        x += Graphics::TextMetrics::measureWidth(truncated(item.label, 32),
                                                 rowFont)
             + 14.0f;

        const auto detail = !item.status.empty()
                                ? truncated(item.status, 48)
                                : truncated(item.detail, 48);
        context.setColor(toColor(theme.ansi[8]));
        context.drawText(detail, {x, baseline}, detailFont);

        if (item.claude)
        {
            const auto badge = std::string {"claude"};
            const auto badgeWidth =
                Graphics::TextMetrics::measureWidth(badge, detailFont);
            context.setColor(toColor(theme.ansi[5]));
            context.drawText(badge,
                             {rowRect.right() - badgeWidth - 12.0f, baseline},
                             detailFont);
        }
    }

    if (visible.empty())
    {
        context.setColor(toColor(theme.ansi[8]));
        context.drawText("no matches",
                         {panel.x + 18.0f,
                          panel.y + headerHeight + rowHeight * 0.62f},
                         rowFont);
    }
}
} // namespace term
