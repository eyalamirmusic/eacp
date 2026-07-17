#pragma once

#include "GlyphAtlas.h"
#include "Pty.h"
#include "TermParser.h"
#include "TermScreen.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/Timer.h>
#include <eacp/Sprites/Sprites.h>

#include <memory>
#include <mutex>
#include <optional>

namespace term
{
// The terminal: a GPU-rendered cell grid over a live PTY. Output is parsed
// into the screen model and drawn from a glyph atlas; every visible pixel is
// composited on the GPU each frame (backgrounds, glyphs, decorations,
// selection, cursor). Repaints are on-demand: PTY output, input, blink.
class TerminalView final : public eacp::GPU::GPUView
{
public:
    TerminalView();
    ~TerminalView() override;

    std::function<void(const std::string&)> onTitleChanged =
        [](const std::string&) {};
    eacp::Callback onShellExit = [] {};

    void render(eacp::GPU::Frame& frame) override;
    void resized() override;

    void keyDown(const eacp::Graphics::KeyEvent& event) override;
    void mouseDown(const eacp::Graphics::MouseEvent& event) override;
    void mouseDragged(const eacp::Graphics::MouseEvent& event) override;
    void mouseUp(const eacp::Graphics::MouseEvent& event) override;
    void mouseWheel(const eacp::Graphics::MouseEvent& event) override;

private:
    struct CellRef
    {
        // Absolute row: scrollback lines count from 0, grid rows follow.
        long row = 0;
        int col = 0;

        bool operator<(const CellRef& other) const
        {
            return row != other.row ? row < other.row : col < other.col;
        }

        bool operator==(const CellRef& other) const
        {
            return row == other.row && col == other.col;
        }
    };

    void flushOutput();
    void send(std::string_view bytes);
    void sendAndScrollToBottom(std::string_view bytes);
    bool handleCommandShortcut(const eacp::Graphics::KeyEvent& event);
    bool handleSpecialKey(const eacp::Graphics::KeyEvent& event);
    void paste();
    void copySelection();
    void setFontSize(float newSize);
    void applyGridSize();
    void scrollBy(int lines);

    bool mouseReportingActive() const;
    void sendMouseReport(const eacp::Graphics::MouseEvent& event,
                         int button,
                         bool pressed,
                         bool motion);

    CellRef cellRefAt(eacp::Graphics::Point pos) const;
    bool isSelected(long absoluteRow, int col) const;
    bool hasSelection() const;
    std::string selectedText() const;

    void drawBackgrounds(int visualRow, const Line& line, float y);
    void drawGlyphs(int visualRow, const Line& line, float y);
    void drawCursor();

    // fg/bg after inverse video and selection are applied.
    std::pair<Rgb, Rgb>
        effectiveColors(const Cell& cell, long absoluteRow, int col) const;

    Theme theme;
    TermScreen screen;
    TermParser parser;
    std::optional<GlyphAtlas> atlas;
    std::optional<eacp::Sprites::SpriteRenderer> sprites;
    Pty pty;

    std::mutex outputLock;
    std::string pendingOutput;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    eacp::Threads::Timer blinkTimer;
    bool blinkOn = true;

    int scrollOffset = 0;
    float fontSize = 13.0f;
    float wheelRemainder = 0;

    bool selecting = false;
    CellRef selectionAnchor;
    CellRef selectionStart;
    CellRef selectionEnd;
    bool selectionActive = false;

    float marginX = 6.0f;
    float marginY = 4.0f;
    std::uint64_t renderedVersion = 0;
};
} // namespace term
