#include "TermScreen.h"

#include <algorithm>

namespace term
{
namespace
{
constexpr int maxScrollback = 10000;

bool inRange(char32_t cp, char32_t lo, char32_t hi)
{
    return cp >= lo && cp <= hi;
}
} // namespace

int charWidth(char32_t cp)
{
    if (cp == 0x200b || cp == 0x200c || cp == 0x200d || cp == 0xfeff)
        return 0;

    if (inRange(cp, 0x0300, 0x036f) || inRange(cp, 0x1ab0, 0x1aff)
        || inRange(cp, 0x1dc0, 0x1dff) || inRange(cp, 0x20d0, 0x20ff)
        || inRange(cp, 0xfe00, 0xfe0f) || inRange(cp, 0xfe20, 0xfe2f))
        return 0;

    if (inRange(cp, 0x1100, 0x115f) || inRange(cp, 0x2e80, 0x303e)
        || inRange(cp, 0x3041, 0x33ff) || inRange(cp, 0x3400, 0x4dbf)
        || inRange(cp, 0x4e00, 0x9fff) || inRange(cp, 0xa000, 0xa4cf)
        || inRange(cp, 0xac00, 0xd7a3) || inRange(cp, 0xf900, 0xfaff)
        || inRange(cp, 0xfe30, 0xfe4f) || inRange(cp, 0xff00, 0xff60)
        || inRange(cp, 0xffe0, 0xffe6) || inRange(cp, 0x1f300, 0x1f64f)
        || inRange(cp, 0x1f680, 0x1f6ff) || inRange(cp, 0x1f900, 0x1f9ff)
        || inRange(cp, 0x20000, 0x2fffd) || inRange(cp, 0x30000, 0x3fffd))
        return 2;

    return 1;
}

TermScreen::TermScreen(int colsToUse, int rowsToUse, const Theme& themeToUse)
    : theme(themeToUse)
{
    pen.fg = theme.foreground;
    pen.bg = theme.background;
    resize(std::max(colsToUse, 1), std::max(rowsToUse, 1));
}

Cell TermScreen::blankCell() const
{
    auto cell = Cell {};
    cell.fg = pen.defaultFg ? theme.foreground : pen.fg;
    cell.bg = pen.defaultBg ? theme.background : pen.bg;
    cell.attrs = (std::uint16_t) ((pen.defaultFg ? Attr::DefaultFg : 0)
                                  | (pen.defaultBg ? Attr::DefaultBg : 0));
    return cell;
}

Line TermScreen::blankLine() const
{
    auto line = Line {};
    line.assign((std::size_t) cols, blankCell());
    return line;
}

void TermScreen::clampCursor()
{
    cursor.x = std::clamp(cursor.x, 0, cols - 1);
    cursor.y = std::clamp(cursor.y, 0, rowCount - 1);
}

void TermScreen::damageWidePair(int x, int y)
{
    auto& line = row(y);
    auto& cell = line[(std::size_t) x];

    if ((cell.attrs & Attr::WideCont) != 0 && x > 0)
    {
        auto& lead = line[(std::size_t) x - 1];
        lead.ch = U' ';
        lead.attrs &= (std::uint16_t) ~Attr::Wide;
    }

    if ((cell.attrs & Attr::Wide) != 0 && x + 1 < cols)
    {
        auto& cont = line[(std::size_t) x + 1];
        cont.ch = U' ';
        cont.attrs &= (std::uint16_t) ~Attr::WideCont;
    }
}

void TermScreen::writeCell(int x, int y, const Cell& cell)
{
    if (x < 0 || x >= cols || y < 0 || y >= rowCount)
        return;

    damageWidePair(x, y);
    row(y)[(std::size_t) x] = cell;
}

void TermScreen::newlineWithScroll()
{
    carriageReturn();
    linefeed();
}

void TermScreen::putChar(char32_t cp)
{
    const auto width = charWidth(cp);

    if (width == 0)
        return;

    if (wrapPending && modes.autowrap)
    {
        newlineWithScroll();
        wrapPending = false;
    }

    // A wide char that no longer fits on the line wraps early.
    if (width == 2 && cursor.x >= cols - 1)
    {
        if (modes.autowrap)
            newlineWithScroll();
        else
            cursor.x = std::max(cols - 2, 0);
    }

    auto cell = blankCell();
    cell.ch = cp;
    cell.fg = pen.defaultFg ? theme.foreground : pen.fg;
    cell.bg = pen.defaultBg ? theme.background : pen.bg;
    cell.attrs = (std::uint16_t) (pen.attrs
                                  | (pen.defaultFg ? Attr::DefaultFg : 0)
                                  | (pen.defaultBg ? Attr::DefaultBg : 0));

    if (width == 2)
    {
        cell.attrs |= Attr::Wide;
        writeCell(cursor.x, cursor.y, cell);

        auto cont = cell;
        cont.ch = U' ';
        cont.attrs =
            (std::uint16_t) ((cell.attrs & ~Attr::Wide) | Attr::WideCont);
        writeCell(cursor.x + 1, cursor.y, cont);
    }
    else
    {
        writeCell(cursor.x, cursor.y, cell);
    }

    lastPrinted = cp;
    cursor.x += width;

    if (cursor.x >= cols)
    {
        cursor.x = cols - 1;
        wrapPending = modes.autowrap;
    }

    markDirty();
}

void TermScreen::repeatLastChar(int count)
{
    if (lastPrinted == 0)
        return;

    for (auto i = 0; i < std::min(count, cols * rowCount); ++i)
        putChar(lastPrinted);
}

void TermScreen::linefeed()
{
    wrapPending = false;

    if (cursor.y == scrollBottom)
        scrollUpRegion(1, scrollTop, scrollBottom);
    else if (cursor.y < rowCount - 1)
        ++cursor.y;

    markDirty();
}

void TermScreen::carriageReturn()
{
    cursor.x = 0;
    wrapPending = false;
    markDirty();
}

void TermScreen::backspace()
{
    wrapPending = false;

    if (cursor.x > 0)
        --cursor.x;

    markDirty();
}

void TermScreen::horizontalTab(int count)
{
    wrapPending = false;

    for (auto i = 0; i < count; ++i)
    {
        auto x = cursor.x + 1;

        while (x < cols - 1 && !tabStops[(std::size_t) x])
            ++x;

        cursor.x = std::min(x, cols - 1);
    }

    markDirty();
}

void TermScreen::backTab(int count)
{
    wrapPending = false;

    for (auto i = 0; i < count; ++i)
    {
        auto x = cursor.x - 1;

        while (x > 0 && !tabStops[(std::size_t) x])
            --x;

        cursor.x = std::max(x, 0);
    }

    markDirty();
}

void TermScreen::reverseIndex()
{
    wrapPending = false;

    if (cursor.y == scrollTop)
        scrollDownRegion(1, scrollTop, scrollBottom);
    else if (cursor.y > 0)
        --cursor.y;

    markDirty();
}

void TermScreen::setTabStop()
{
    tabStops[(std::size_t) cursor.x] = true;
}

void TermScreen::clearTabStops(int mode)
{
    if (mode == 0)
        tabStops[(std::size_t) cursor.x] = false;
    else if (mode == 3)
        std::fill(tabStops.begin(), tabStops.end(), false);
}

void TermScreen::setCursor(int col, int row)
{
    wrapPending = false;
    cursor.x = col;
    cursor.y = modes.origin ? scrollTop + row : row;

    if (modes.origin)
        cursor.y = std::clamp(cursor.y, scrollTop, scrollBottom);

    clampCursor();
    markDirty();
}

void TermScreen::setColumn(int col)
{
    wrapPending = false;
    cursor.x = std::clamp(col, 0, cols - 1);
    markDirty();
}

void TermScreen::setRow(int rowToUse)
{
    setCursor(cursor.x, rowToUse);
}

void TermScreen::moveCursor(int dx, int dy)
{
    wrapPending = false;
    cursor.x = std::clamp(cursor.x + dx, 0, cols - 1);

    // Relative vertical motion stays inside the scroll region when the cursor
    // starts inside it, matching DEC semantics.
    const auto top = cursor.y >= scrollTop ? scrollTop : 0;
    const auto bottom = cursor.y <= scrollBottom ? scrollBottom : rowCount - 1;
    cursor.y = std::clamp(cursor.y + dy, top, bottom);
    markDirty();
}

void TermScreen::saveCursor()
{
    savedCursor = cursor;
    savedPen = pen;
}

void TermScreen::restoreCursor()
{
    cursor = savedCursor;
    pen = savedPen;
    wrapPending = false;
    clampCursor();
    markDirty();
}

void TermScreen::eraseInDisplay(int mode)
{
    wrapPending = false;

    if (mode == 3)
    {
        scrollback.clear();
        eraseInDisplay(2);
        return;
    }

    const auto blank = blankCell();

    auto clearLine = [&](int y, int fromX, int toX)
    {
        auto& line = row(y);

        for (auto x = std::max(fromX, 0); x <= std::min(toX, cols - 1); ++x)
            line[(std::size_t) x] = blank;
    };

    if (mode == 0)
    {
        clearLine(cursor.y, cursor.x, cols - 1);

        for (auto y = cursor.y + 1; y < rowCount; ++y)
            clearLine(y, 0, cols - 1);
    }
    else if (mode == 1)
    {
        for (auto y = 0; y < cursor.y; ++y)
            clearLine(y, 0, cols - 1);

        clearLine(cursor.y, 0, cursor.x);
    }
    else if (mode == 2)
    {
        for (auto y = 0; y < rowCount; ++y)
            clearLine(y, 0, cols - 1);
    }

    markDirty();
}

void TermScreen::eraseInLine(int mode)
{
    wrapPending = false;
    const auto blank = blankCell();
    auto& line = row(cursor.y);

    auto from = mode == 0 ? cursor.x : 0;
    auto to = mode == 1 ? cursor.x : cols - 1;

    for (auto x = from; x <= to; ++x)
        line[(std::size_t) x] = blank;

    markDirty();
}

void TermScreen::insertLines(int count)
{
    if (cursor.y < scrollTop || cursor.y > scrollBottom)
        return;

    scrollDownRegion(count, cursor.y, scrollBottom);
    cursor.x = 0;
    markDirty();
}

void TermScreen::deleteLines(int count)
{
    if (cursor.y < scrollTop || cursor.y > scrollBottom)
        return;

    scrollUpRegion(count, cursor.y, scrollBottom);
    cursor.x = 0;
    markDirty();
}

void TermScreen::insertChars(int count)
{
    wrapPending = false;
    auto& line = row(cursor.y);
    const auto n = std::clamp(count, 1, cols - cursor.x);

    damageWidePair(cursor.x, cursor.y);
    line.insert(line.begin() + cursor.x, (std::size_t) n, blankCell());
    line.resize((std::size_t) cols);
    markDirty();
}

void TermScreen::deleteChars(int count)
{
    wrapPending = false;
    auto& line = row(cursor.y);
    const auto n = std::clamp(count, 1, cols - cursor.x);

    damageWidePair(cursor.x, cursor.y);
    line.erase(line.begin() + cursor.x, line.begin() + cursor.x + n);
    line.resize((std::size_t) cols, blankCell());
    markDirty();
}

void TermScreen::eraseChars(int count)
{
    wrapPending = false;
    auto& line = row(cursor.y);
    const auto n = std::clamp(count, 1, cols - cursor.x);
    const auto blank = blankCell();

    for (auto x = cursor.x; x < cursor.x + n; ++x)
    {
        damageWidePair(x, cursor.y);
        line[(std::size_t) x] = blank;
    }

    markDirty();
}

void TermScreen::scrollUp(int count)
{
    scrollUpRegion(count, scrollTop, scrollBottom);
}

void TermScreen::scrollDown(int count)
{
    scrollDownRegion(count, scrollTop, scrollBottom);
}

void TermScreen::scrollUpRegion(int count, int top, int bottom)
{
    const auto n = std::clamp(count, 1, bottom - top + 1);
    auto& lines = activeGrid();

    for (auto i = 0; i < n; ++i)
    {
        // Only lines leaving the top of a full-width primary screen become
        // history; region scrolls and the alt screen just discard.
        if (top == 0 && !modes.altScreen)
        {
            scrollback.push_back(std::move(lines[(std::size_t) top]));

            if ((int) scrollback.size() > maxScrollback)
                scrollback.pop_front();
        }

        lines.erase(lines.begin() + top);
        lines.insert(lines.begin() + bottom, blankLine());
    }

    markDirty();
}

void TermScreen::scrollDownRegion(int count, int top, int bottom)
{
    const auto n = std::clamp(count, 1, bottom - top + 1);
    auto& lines = activeGrid();

    for (auto i = 0; i < n; ++i)
    {
        lines.erase(lines.begin() + bottom);
        lines.insert(lines.begin() + top, blankLine());
    }

    markDirty();
}

void TermScreen::setScrollRegion(int top, int bottom)
{
    if (bottom <= top)
    {
        scrollTop = 0;
        scrollBottom = rowCount - 1;
    }
    else
    {
        scrollTop = std::clamp(top, 0, rowCount - 1);
        scrollBottom = std::clamp(bottom, scrollTop, rowCount - 1);
    }

    setCursor(0, 0);
}

void TermScreen::useAltScreen(bool on, bool saveCursorAndClear)
{
    if (on == modes.altScreen)
        return;

    if (on)
    {
        if (saveCursorAndClear)
            saveCursor();

        modes.altScreen = true;

        if (saveCursorAndClear)
        {
            for (auto& line: altGrid)
                std::fill(line.begin(), line.end(), blankCell());

            cursor = {0, 0};
        }
    }
    else
    {
        modes.altScreen = false;

        if (saveCursorAndClear)
            restoreCursor();
    }

    wrapPending = false;
    markDirty();
}

void TermScreen::fullReset()
{
    pen = Pen {};
    pen.fg = theme.foreground;
    pen.bg = theme.background;
    modes = Modes {};
    cursor = {0, 0};
    savedCursor = {0, 0};
    savedPen = pen;
    wrapPending = false;
    scrollTop = 0;
    scrollBottom = rowCount - 1;

    for (auto& line: grid)
        std::fill(line.begin(), line.end(), blankCell());

    for (auto& line: altGrid)
        std::fill(line.begin(), line.end(), blankCell());

    tabStops.assign((std::size_t) cols, false);

    for (auto x = 8; x < cols; x += 8)
        tabStops[(std::size_t) x] = true;

    markDirty();
}

void TermScreen::resize(int newCols, int newRows)
{
    newCols = std::max(newCols, 2);
    newRows = std::max(newRows, 1);

    if (newCols == cols && newRows == rowCount && !grid.empty())
        return;

    cols = newCols;

    for (auto* lines: {&grid, &altGrid})
    {
        // Shrink: overflow above the cursor becomes history so content is
        // not lost. Grow: pull history back onto the screen.
        while ((int) lines->size() > newRows)
        {
            if (cursor.y > 0 && lines == &grid)
            {
                scrollback.push_back(std::move(lines->front()));
                lines->erase(lines->begin());
                --cursor.y;
            }
            else
            {
                lines->pop_back();
            }
        }

        while ((int) lines->size() < newRows)
        {
            if (lines == &grid && !scrollback.empty())
            {
                lines->insert(lines->begin(), std::move(scrollback.back()));
                scrollback.pop_back();
                ++cursor.y;
            }
            else
            {
                lines->emplace_back();
            }
        }

        for (auto& line: *lines)
            line.resize((std::size_t) cols, blankCell());
    }

    rowCount = newRows;
    scrollTop = 0;
    scrollBottom = rowCount - 1;
    wrapPending = false;

    auto oldStops = tabStops;
    tabStops.assign((std::size_t) cols, false);

    for (auto x = 0; x < cols; ++x)
        if (x < (int) oldStops.size() ? oldStops[(std::size_t) x] : x % 8 == 0)
            tabStops[(std::size_t) x] = x > 0;

    clampCursor();
    markDirty();
}

const Line& TermScreen::lineAt(int visualRow, int scrollOffset) const
{
    const auto& lines = activeGrid();
    const auto index =
        (std::int64_t) scrollback.size() - scrollOffset + visualRow;

    if (index < 0)
        return lines[0];

    if (index < (std::int64_t) scrollback.size())
        return scrollback[(std::size_t) index];

    const auto gridIndex = std::min((std::size_t) (index - scrollback.size()),
                                    lines.size() - 1);
    return lines[gridIndex];
}
} // namespace term
