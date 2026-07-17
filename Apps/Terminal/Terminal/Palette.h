#pragma once

#include "Config.h"
#include "Session.h"

#include <string>
#include <vector>

namespace term
{
struct PaletteItem
{
    enum class Kind
    {
        Session,
        Project
    };

    Kind kind = Kind::Project;
    std::string key;
    std::string label;
    std::string detail;
    std::string status;
    bool claude = false;
    std::int64_t lastUsed = 0;
    TermSession* session = nullptr;
};

// The switcher: one overlay that fuzzy-searches everything — open sessions
// (Claude sessions surfaced with their conversation title and last notify),
// then known project dirs that aren't open yet. Empty query lists by
// recency, Wim-style; Enter switches or spawns. CPU-painted chrome over the
// GPU terminal.
class Palette final : public eacp::Graphics::View
{
public:
    Palette(const AppConfig& configToUse, SessionManager& sessionsToUse);

    void show();
    bool isShown() const { return shown; }

    // Fired for both dismissal and selection; the shell removes the overlay
    // and restores terminal focus.
    eacp::Callback onClosed = [] {};

    void paint(eacp::Graphics::Context& context) override;
    void keyDown(const eacp::Graphics::KeyEvent& event) override;
    void mouseDown(const eacp::Graphics::MouseEvent& event) override;
    void mouseMoved(const eacp::Graphics::MouseEvent& event) override;

private:
    void rebuild();
    void applyQuery();
    void choose();
    void moveSelection(int delta);
    void popQueryChar();
    int rowAt(eacp::Graphics::Point pos) const;
    eacp::Graphics::Rect panelBounds() const;

    const AppConfig& config;
    SessionManager& sessions;
    Theme theme;

    std::vector<PaletteItem> allItems;
    std::vector<PaletteItem> visible;
    std::string query;
    int selected = 0;
    bool shown = false;

    eacp::Graphics::Font queryFont;
    eacp::Graphics::Font rowFont;
    eacp::Graphics::Font detailFont;
};
} // namespace term
