#include "TerminalView.h"

#include <eacp/Core/App/Clipboard.h>

#include <algorithm>
#include <cstdio>

namespace term
{
using namespace eacp;
namespace KeyCode = Graphics::KeyCode;
using Graphics::KeyEvent;
using Graphics::MouseEvent;

namespace
{
constexpr auto fontName = "Menlo";

namespace MacKey
{
constexpr std::uint16_t Home = 0x73;
constexpr std::uint16_t End = 0x77;
constexpr std::uint16_t PageUp = 0x74;
constexpr std::uint16_t PageDown = 0x79;
constexpr std::uint16_t ForwardDelete = 0x75;
constexpr std::uint16_t KeypadEnter = 0x4c;
} // namespace MacKey

int modifierCode(const Graphics::ModifierKeys& mods)
{
    return 1 + (mods.shift ? 1 : 0) + (mods.alt ? 2 : 0) + (mods.control ? 4 : 0);
}

std::string csiWithModifiers(char final, const Graphics::ModifierKeys& mods)
{
    const auto code = modifierCode(mods);

    if (code == 1)
        return std::string {"\033["} + final;

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "\033[1;%d%c", code, final);
    return buffer;
}

std::string tildeKey(int number, const Graphics::ModifierKeys& mods)
{
    char buffer[16];
    const auto code = modifierCode(mods);

    if (code == 1)
        std::snprintf(buffer, sizeof(buffer), "\033[%d~", number);
    else
        std::snprintf(buffer, sizeof(buffer), "\033[%d;%d~", number, code);

    return buffer;
}

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80)
    {
        out.push_back((char) cp);
    }
    else if (cp < 0x800)
    {
        out.push_back((char) (0xc0 | (cp >> 6)));
        out.push_back((char) (0x80 | (cp & 0x3f)));
    }
    else if (cp < 0x10000)
    {
        out.push_back((char) (0xe0 | (cp >> 12)));
        out.push_back((char) (0x80 | ((cp >> 6) & 0x3f)));
        out.push_back((char) (0x80 | (cp & 0x3f)));
    }
    else
    {
        out.push_back((char) (0xf0 | (cp >> 18)));
        out.push_back((char) (0x80 | ((cp >> 12) & 0x3f)));
        out.push_back((char) (0x80 | ((cp >> 6) & 0x3f)));
        out.push_back((char) (0x80 | (cp & 0x3f)));
    }
}
} // namespace

TerminalView::TerminalView()
    : screen(80, 24, theme)
    , parser(screen, theme)
    , blinkTimer(
          [this]
          {
              blinkOn = !blinkOn;
              repaint();
          },
          2)
{
    setSampleCount(1);
    setHandlesMouseEvents(true);
    setGrabsFocusOnMouseDown(true);

    atlas.emplace(fontName, fontSize);

    parser.respond = [this](std::string_view bytes) { send(bytes); };
    parser.onTitleChanged = [this](const std::string& title)
    { onTitleChanged(title); };

    auto guard = std::weak_ptr<bool> {alive};

    pty.start(
        {screen.columns(), screen.rows()},
        [this, guard](const std::string& data)
        {
            {
                auto lock = std::scoped_lock {outputLock};
                pendingOutput += data;
            }

            Threads::callAsync(
                [this, guard]
                {
                    if (!guard.expired())
                        flushOutput();
                });
        },
        [this, guard]
        {
            Threads::callAsync(
                [this, guard]
                {
                    if (!guard.expired())
                        onShellExit();
                });
        });
}

TerminalView::~TerminalView()
{
    *alive = false;
    alive.reset();
    pty.shutdown();
}

void TerminalView::flushOutput()
{
    auto data = std::string {};

    {
        auto lock = std::scoped_lock {outputLock};
        data.swap(pendingOutput);
    }

    if (data.empty())
        return;

    parser.feed(data);

    if (screen.modes.altScreen)
        scrollOffset = 0;

    scrollOffset = std::min(scrollOffset, screen.scrollbackSize());
    repaint();
}

void TerminalView::send(std::string_view bytes)
{
    pty.write(bytes);
}

void TerminalView::sendAndScrollToBottom(std::string_view bytes)
{
    scrollOffset = 0;
    blinkOn = true;
    send(bytes);
    repaint();
}

void TerminalView::resized()
{
    GPUView::resized();

    const auto bounds = getLocalBounds();

    if (bounds.w > 0 && bounds.h > 0)
        sprites.emplace(Graphics::Point {bounds.w, bounds.h}, sampleCount());

    applyGridSize();
    repaint();
}

void TerminalView::applyGridSize()
{
    const auto bounds = getLocalBounds();
    const auto cellW = atlas->cellWidth();
    const auto cellH = atlas->cellHeight();

    if (cellW <= 0 || cellH <= 0 || bounds.w <= 0 || bounds.h <= 0)
        return;

    const auto cols = std::max(2, (int) ((bounds.w - 2 * marginX) / cellW));
    const auto rows = std::max(1, (int) ((bounds.h - 2 * marginY) / cellH));

    if (cols == screen.columns() && rows == screen.rows())
        return;

    screen.resize(cols, rows);
    pty.resize({cols, rows});
    scrollOffset = std::min(scrollOffset, screen.scrollbackSize());
}

void TerminalView::setFontSize(float newSize)
{
    const auto clamped = std::clamp(newSize, 7.0f, 40.0f);

    if (clamped == fontSize)
        return;

    fontSize = clamped;
    atlas.emplace(fontName, fontSize);
    applyGridSize();
    repaint();
}

void TerminalView::scrollBy(int lines)
{
    const auto previous = scrollOffset;
    scrollOffset = std::clamp(scrollOffset + lines, 0, screen.scrollbackSize());

    if (scrollOffset != previous)
        repaint();
}

// ---- rendering -------------------------------------------------------------

std::pair<Rgb, Rgb>
    TerminalView::effectiveColors(const Cell& cell, long absoluteRow, int col) const
{
    auto fg = cell.fg;
    auto bg = cell.bg;

    if ((cell.attrs & Attr::Inverse) != 0)
        std::swap(fg, bg);

    if (isSelected(absoluteRow, col))
        bg = theme.selection;

    return {fg, bg};
}

void TerminalView::drawBackgrounds(int visualRow, const Line& line, float y)
{
    const auto cellW = atlas->cellWidth();
    const auto cellH = atlas->cellHeight();
    const auto absoluteRow =
        (long) screen.scrollbackSize() - scrollOffset + visualRow;

    const auto cols = std::min((int) line.size(), screen.columns());
    auto runStart = 0;
    auto runBg = Rgb {};
    auto runActive = false;

    auto flush = [&](int endCol)
    {
        if (!runActive)
            return;

        sprites->fillRect({marginX + (float) runStart * cellW,
                           y,
                           (float) (endCol - runStart) * cellW,
                           cellH},
                          toColor(runBg));
        runActive = false;
    };

    for (auto col = 0; col < cols; ++col)
    {
        const auto& cell = line[(std::size_t) col];
        const auto [fg, bg] = effectiveColors(cell, absoluteRow, col);

        const auto isDefault = bg == theme.background
                               && (cell.attrs & Attr::Inverse) == 0
                               && !isSelected(absoluteRow, col);

        if (isDefault)
        {
            flush(col);
            continue;
        }

        if (runActive && bg == runBg)
            continue;

        flush(col);
        runStart = col;
        runBg = bg;
        runActive = true;
    }

    flush(cols);
}

void TerminalView::drawGlyphs(int visualRow, const Line& line, float y)
{
    const auto cellW = atlas->cellWidth();
    const auto cellH = atlas->cellHeight();
    const auto baseline = atlas->baseline();
    const auto absoluteRow =
        (long) screen.scrollbackSize() - scrollOffset + visualRow;

    const auto cols = std::min((int) line.size(), screen.columns());

    for (auto col = 0; col < cols; ++col)
    {
        const auto& cell = line[(std::size_t) col];

        if ((cell.attrs & Attr::WideCont) != 0)
            continue;

        if ((cell.attrs & Attr::Hidden) != 0)
            continue;

        if ((cell.attrs & Attr::Blink) != 0 && !blinkOn)
            continue;

        const auto [fg, bg] = effectiveColors(cell, absoluteRow, col);
        const auto x = marginX + (float) col * cellW;
        const auto wide = (cell.attrs & Attr::Wide) != 0;
        const auto width = wide ? cellW * 2 : cellW;

        if (cell.ch != U' ')
        {
            const auto& slot = atlas->glyph(cell.ch,
                                            (cell.attrs & Attr::Bold) != 0,
                                            (cell.attrs & Attr::Italic) != 0);

            if (slot.valid)
            {
                const auto alpha = (cell.attrs & Attr::Faint) != 0 ? 0.55f : 1.0f;
                const auto tint = slot.colored ? Graphics::Color::white(alpha)
                                               : toColor(fg, alpha);

                sprites->drawTexture(
                    atlas->texture(), slot.src, {x, y, width, cellH}, tint);
            }
        }

        if ((cell.attrs & Attr::Underline) != 0)
            sprites->fillRect({x, y + baseline + 1.5f, width, 1.0f}, toColor(fg));

        if ((cell.attrs & Attr::Strike) != 0)
            sprites->fillRect({x, y + cellH * 0.55f, width, 1.0f}, toColor(fg));
    }
}

void TerminalView::drawCursor()
{
    if (!screen.modes.showCursor)
        return;

    const auto visualRow = screen.cursor.y + scrollOffset;

    if (visualRow >= screen.rows())
        return;

    const auto cellW = atlas->cellWidth();
    const auto cellH = atlas->cellHeight();
    const auto x = marginX + (float) screen.cursor.x * cellW;
    const auto y = marginY + (float) visualRow * cellH;
    const auto color = toColor(theme.cursor);

    const auto shape = parser.cursorShape();
    const auto blinking = screen.modes.cursorBlink && (shape == 0 || shape % 2 == 1);

    if (!hasFocus())
    {
        sprites->drawRect({x, y, cellW, cellH}, color, 1.0f);
        return;
    }

    if (blinking && !blinkOn)
        return;

    if (shape == 3 || shape == 4)
    {
        sprites->fillRect({x, y + cellH - 2.0f, cellW, 2.0f}, color);
        return;
    }

    if (shape == 5 || shape == 6)
    {
        sprites->fillRect({x, y, 2.0f, cellH}, color);
        return;
    }

    sprites->fillRect({x, y, cellW, cellH}, color);

    // Repaint the glyph under a block cursor in the background color so it
    // stays legible.
    const auto& line = screen.lineAt(visualRow, scrollOffset);

    if (screen.cursor.x < (int) line.size())
    {
        const auto& cell = line[(std::size_t) screen.cursor.x];

        if (cell.ch != U' ' && (cell.attrs & Attr::WideCont) == 0)
        {
            const auto& slot = atlas->glyph(cell.ch,
                                            (cell.attrs & Attr::Bold) != 0,
                                            (cell.attrs & Attr::Italic) != 0);

            if (slot.valid && !slot.colored)
            {
                const auto wide = (cell.attrs & Attr::Wide) != 0;
                sprites->drawTexture(atlas->texture(),
                                     slot.src,
                                     {x, y, wide ? cellW * 2 : cellW, cellH},
                                     toColor(theme.background));
            }
        }
    }
}

void TerminalView::render(GPU::Frame& frame)
{
    auto pass = frame.beginPass({toColor(theme.background)});

    if (!sprites)
        return;

    sprites->begin(pass);

    const auto rows = screen.rows();
    const auto cellH = atlas->cellHeight();

    // Rasterize every glyph this frame needs before the first draw call so
    // the atlas texture uploads once, ahead of the draws that sample it.
    for (auto row = 0; row < rows; ++row)
    {
        const auto& line = screen.lineAt(row, scrollOffset);
        const auto cols = std::min((int) line.size(), screen.columns());

        for (auto col = 0; col < cols; ++col)
        {
            const auto& cell = line[(std::size_t) col];

            if (cell.ch != U' '
                && (cell.attrs & (Attr::WideCont | Attr::Hidden)) == 0)
                atlas->glyph(cell.ch,
                             (cell.attrs & Attr::Bold) != 0,
                             (cell.attrs & Attr::Italic) != 0);
        }
    }

    atlas->texture();

    for (auto row = 0; row < rows; ++row)
        drawBackgrounds(
            row, screen.lineAt(row, scrollOffset), marginY + (float) row * cellH);

    for (auto row = 0; row < rows; ++row)
        drawGlyphs(
            row, screen.lineAt(row, scrollOffset), marginY + (float) row * cellH);

    if (scrollOffset == 0)
        drawCursor();

    renderedVersion = screen.version();
}

// ---- keyboard --------------------------------------------------------------

bool TerminalView::handleCommandShortcut(const KeyEvent& event)
{
    if (!event.modifiers.command)
        return false;

    const auto& chars = event.charactersIgnoringModifiers;

    if (chars == "c")
    {
        copySelection();
        return true;
    }

    if (chars == "v")
    {
        paste();
        return true;
    }

    if (chars == "k")
    {
        screen.eraseInDisplay(3);
        screen.setCursor(0, 0);
        scrollOffset = 0;
        send("\014");
        repaint();
        return true;
    }

    if (chars == "=" || chars == "+")
    {
        setFontSize(fontSize + 1);
        return true;
    }

    if (chars == "-")
    {
        setFontSize(fontSize - 1);
        return true;
    }

    if (chars == "0")
    {
        setFontSize(13.0f);
        return true;
    }

    return true;
}

bool TerminalView::handleSpecialKey(const KeyEvent& event)
{
    const auto& mods = event.modifiers;
    const auto app = screen.modes.appCursorKeys && modifierCode(mods) == 1;

    switch (event.keyCode)
    {
        case KeyCode::Return:
        case MacKey::KeypadEnter:
            sendAndScrollToBottom("\r");
            return true;

        case KeyCode::Tab:
            sendAndScrollToBottom(mods.shift ? "\033[Z" : "\t");
            return true;

        case KeyCode::Escape:
            sendAndScrollToBottom("\033");
            return true;

        case KeyCode::Delete:
            sendAndScrollToBottom(mods.alt ? "\033\x7f" : "\x7f");
            return true;

        case MacKey::ForwardDelete:
            sendAndScrollToBottom(tildeKey(3, mods));
            return true;

        case KeyCode::UpArrow:
            sendAndScrollToBottom(app ? "\033OA" : csiWithModifiers('A', mods));
            return true;

        case KeyCode::DownArrow:
            sendAndScrollToBottom(app ? "\033OB" : csiWithModifiers('B', mods));
            return true;

        case KeyCode::RightArrow:
            sendAndScrollToBottom(app ? "\033OC" : csiWithModifiers('C', mods));
            return true;

        case KeyCode::LeftArrow:
            sendAndScrollToBottom(app ? "\033OD" : csiWithModifiers('D', mods));
            return true;

        case MacKey::Home:
            if (mods.shift)
                scrollBy(screen.scrollbackSize());
            else
                sendAndScrollToBottom(app ? "\033OH" : "\033[H");
            return true;

        case MacKey::End:
            if (mods.shift)
                scrollBy(-screen.scrollbackSize());
            else
                sendAndScrollToBottom(app ? "\033OF" : "\033[F");
            return true;

        case MacKey::PageUp:
            if (mods.shift)
                scrollBy(screen.rows() - 1);
            else
                sendAndScrollToBottom(tildeKey(5, mods));
            return true;

        case MacKey::PageDown:
            if (mods.shift)
                scrollBy(-(screen.rows() - 1));
            else
                sendAndScrollToBottom(tildeKey(6, mods));
            return true;

        case KeyCode::F1:
            sendAndScrollToBottom("\033OP");
            return true;
        case KeyCode::F2:
            sendAndScrollToBottom("\033OQ");
            return true;
        case KeyCode::F3:
            sendAndScrollToBottom("\033OR");
            return true;
        case KeyCode::F4:
            sendAndScrollToBottom("\033OS");
            return true;
        case KeyCode::F5:
            sendAndScrollToBottom(tildeKey(15, mods));
            return true;
        case KeyCode::F6:
            sendAndScrollToBottom(tildeKey(17, mods));
            return true;
        case KeyCode::F7:
            sendAndScrollToBottom(tildeKey(18, mods));
            return true;
        case KeyCode::F8:
            sendAndScrollToBottom(tildeKey(19, mods));
            return true;
        case KeyCode::F9:
            sendAndScrollToBottom(tildeKey(20, mods));
            return true;
        case KeyCode::F10:
            sendAndScrollToBottom(tildeKey(21, mods));
            return true;
        case KeyCode::F11:
            sendAndScrollToBottom(tildeKey(23, mods));
            return true;
        case KeyCode::F12:
            sendAndScrollToBottom(tildeKey(24, mods));
            return true;

        default:
            return false;
    }
}

void TerminalView::keyDown(const KeyEvent& event)
{
    if (handleCommandShortcut(event))
        return;

    if (handleSpecialKey(event))
        return;

    const auto& mods = event.modifiers;

    if (mods.control && event.charactersIgnoringModifiers == " ")
    {
        sendAndScrollToBottom(std::string_view {"\0", 1});
        return;
    }

    if (mods.alt && !event.charactersIgnoringModifiers.empty())
    {
        sendAndScrollToBottom("\033" + event.charactersIgnoringModifiers);
        return;
    }

    if (!event.characters.empty())
        sendAndScrollToBottom(event.characters);
}

void TerminalView::paste()
{
    auto text = Clipboard::getText();

    if (text.empty())
        return;

    auto normalized = std::string {};
    normalized.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue;

        normalized.push_back(text[i] == '\n' ? '\r' : text[i]);
    }

    if (screen.modes.bracketedPaste)
        normalized = "\033[200~" + normalized + "\033[201~";

    sendAndScrollToBottom(normalized);
}

// ---- selection & mouse -----------------------------------------------------

TerminalView::CellRef TerminalView::cellRefAt(Graphics::Point pos) const
{
    const auto cellW = atlas->cellWidth();
    const auto cellH = atlas->cellHeight();

    const auto col =
        std::clamp((int) ((pos.x - marginX) / cellW), 0, screen.columns() - 1);
    const auto visualRow =
        std::clamp((int) ((pos.y - marginY) / cellH), 0, screen.rows() - 1);

    return {(long) screen.scrollbackSize() - scrollOffset + visualRow, col};
}

bool TerminalView::hasSelection() const
{
    return selectionActive && !(selectionStart == selectionEnd && !selecting);
}

bool TerminalView::isSelected(long absoluteRow, int col) const
{
    if (!selectionActive)
        return false;

    const auto cell = CellRef {absoluteRow, col};
    return !(cell < selectionStart) && !(selectionEnd < cell);
}

std::string TerminalView::selectedText() const
{
    if (!selectionActive)
        return {};

    auto result = std::string {};

    for (auto row = selectionStart.row; row <= selectionEnd.row; ++row)
    {
        const auto offset = (int) (screen.scrollbackSize() - row);

        if (offset < 0 || offset > screen.scrollbackSize())
            continue;

        const auto& line = screen.lineAt(0, offset);
        const auto fromCol = row == selectionStart.row ? selectionStart.col : 0;
        const auto toCol =
            row == selectionEnd.row ? selectionEnd.col : (int) line.size() - 1;

        auto text = std::string {};

        for (auto col = fromCol; col <= std::min(toCol, (int) line.size() - 1);
             ++col)
        {
            const auto& cell = line[(std::size_t) col];

            if ((cell.attrs & Attr::WideCont) != 0)
                continue;

            appendUtf8(text, cell.ch);
        }

        while (!text.empty() && text.back() == ' ')
            text.pop_back();

        result += text;

        if (row != selectionEnd.row)
            result.push_back('\n');
    }

    return result;
}

void TerminalView::copySelection()
{
    if (hasSelection())
        Clipboard::copyText(selectedText());
}

bool TerminalView::mouseReportingActive() const
{
    const auto& modes = screen.modes;
    return modes.mouseButtons || modes.mouseDrag || modes.mouseMotion;
}

void TerminalView::sendMouseReport(const MouseEvent& event,
                                   int button,
                                   bool pressed,
                                   bool motion)
{
    if (!screen.modes.mouseSgr)
        return;

    const auto cellW = atlas->cellWidth();
    const auto cellH = atlas->cellHeight();
    const auto col =
        std::clamp((int) ((event.pos.x - marginX) / cellW) + 1, 1, screen.columns());
    const auto row =
        std::clamp((int) ((event.pos.y - marginY) / cellH) + 1, 1, screen.rows());

    char buffer[32];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "\033[<%d;%d;%d%c",
                  button + (motion ? 32 : 0),
                  col,
                  row,
                  pressed ? 'M' : 'm');
    send(buffer);
}

void TerminalView::mouseDown(const MouseEvent& event)
{
    if (mouseReportingActive() && !event.modifiers.shift)
    {
        sendMouseReport(event, (int) event.button, true, false);
        return;
    }

    if (event.button != Graphics::MouseButton::Left)
        return;

    const auto cell = cellRefAt(event.pos);

    if (event.clickCount >= 3)
    {
        selectionStart = {cell.row, 0};
        selectionEnd = {cell.row, screen.columns() - 1};
        selectionActive = true;
        selecting = false;
        repaint();
        return;
    }

    if (event.clickCount == 2)
    {
        const auto offset = (int) (screen.scrollbackSize() - cell.row);
        const auto& line = screen.lineAt(0, offset);

        auto isWordChar = [&](int col)
        {
            if (col < 0 || col >= (int) line.size())
                return false;

            const auto ch = line[(std::size_t) col].ch;
            return ch != U' ';
        };

        auto from = cell.col;
        auto to = cell.col;

        while (isWordChar(from - 1))
            --from;

        while (isWordChar(to + 1))
            ++to;

        selectionStart = {cell.row, from};
        selectionEnd = {cell.row, to};
        selectionActive = isWordChar(cell.col);
        selecting = false;
        repaint();
        return;
    }

    selectionAnchor = cell;
    selectionStart = cell;
    selectionEnd = cell;
    selectionActive = false;
    selecting = true;
    repaint();
}

void TerminalView::mouseDragged(const MouseEvent& event)
{
    if (mouseReportingActive() && !event.modifiers.shift)
    {
        if (screen.modes.mouseDrag || screen.modes.mouseMotion)
            sendMouseReport(event, (int) event.button, true, true);

        return;
    }

    if (!selecting)
        return;

    const auto cell = cellRefAt(event.pos);
    selectionStart = std::min(selectionAnchor, cell);
    selectionEnd = std::max(selectionAnchor, cell);
    selectionActive = true;
    repaint();
}

void TerminalView::mouseUp(const MouseEvent& event)
{
    if (mouseReportingActive() && !event.modifiers.shift)
    {
        sendMouseReport(event, (int) event.button, false, false);
        return;
    }

    if (selecting && selectionStart == selectionEnd)
        selectionActive = false;

    selecting = false;
    repaint();
}

void TerminalView::mouseWheel(const MouseEvent& event)
{
    if (mouseReportingActive() && !event.modifiers.shift)
    {
        const auto up = event.delta.y > 0;
        sendMouseReport(event, up ? 64 : 65, true, false);
        return;
    }

    wheelRemainder += event.delta.y;
    const auto lines = (int) wheelRemainder;

    if (lines == 0)
        return;

    wheelRemainder -= (float) lines;

    // Full-screen apps get wheel motion as arrow keys, the classic fallback
    // when no mouse mode is active.
    if (screen.modes.altScreen)
    {
        const auto key = lines > 0
                             ? (screen.modes.appCursorKeys ? "\033OA" : "\033[A")
                             : (screen.modes.appCursorKeys ? "\033OB" : "\033[B");

        for (auto i = 0; i < std::min(std::abs(lines), 40); ++i)
            send(key);

        return;
    }

    scrollBy(lines);
}
} // namespace term
