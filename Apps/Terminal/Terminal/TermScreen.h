#pragma once

#include "TermTypes.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace term
{
using Line = std::vector<Cell>;

struct CursorPos
{
    int x = 0;
    int y = 0;
};

struct Pen
{
    Rgb fg = 0;
    Rgb bg = 0;
    std::uint16_t attrs = 0;
    bool defaultFg = true;
    bool defaultBg = true;
};

struct Modes
{
    bool autowrap = true;
    bool origin = false;
    bool appCursorKeys = false;
    bool appKeypad = false;
    bool showCursor = true;
    bool cursorBlink = true;
    bool bracketedPaste = false;
    bool altScreen = false;
    bool mouseButtons = false;
    bool mouseDrag = false;
    bool mouseMotion = false;
    bool mouseSgr = false;
};

// The terminal's cell grid: primary + alternate buffers, scrollback, cursor,
// scroll region and the erase/insert/delete operations the parser dispatches.
// All coordinates are 0-based; erases and scrolls fill with the pen's
// background (BCE), which full-screen apps rely on.
class TermScreen
{
public:
    TermScreen(int colsToUse, int rowsToUse, const Theme& themeToUse);

    int columns() const { return cols; }
    int rows() const { return rowCount; }

    void putChar(char32_t cp);
    void repeatLastChar(int count);

    void linefeed();
    void carriageReturn();
    void backspace();
    void horizontalTab(int count = 1);
    void backTab(int count);
    void reverseIndex();
    void setTabStop();
    void clearTabStops(int mode);

    void setCursor(int col, int row);
    void setColumn(int col);
    void setRow(int row);
    void moveCursor(int dx, int dy);
    void saveCursor();
    void restoreCursor();

    void eraseInDisplay(int mode);
    void eraseInLine(int mode);
    void insertLines(int count);
    void deleteLines(int count);
    void insertChars(int count);
    void deleteChars(int count);
    void eraseChars(int count);
    void scrollUp(int count);
    void scrollDown(int count);
    void setScrollRegion(int top, int bottom);

    void useAltScreen(bool on, bool saveCursorAndClear);
    void fullReset();
    void resize(int newCols, int newRows);

    int scrollbackSize() const { return (int) scrollback.size(); }
    const Line& lineAt(int visualRow, int scrollOffset) const;

    // Bumped on every visible change; the view repaints when it moves.
    std::uint64_t version() const { return changeVersion; }

    Pen pen;
    Modes modes;
    CursorPos cursor;

private:
    Line& row(int index) { return activeGrid()[(std::size_t) index]; }
    std::vector<Line>& activeGrid() { return modes.altScreen ? altGrid : grid; }
    const std::vector<Line>& activeGrid() const
    {
        return modes.altScreen ? altGrid : grid;
    }

    Cell blankCell() const;
    Line blankLine() const;
    void markDirty() { ++changeVersion; }
    void clampCursor();
    void writeCell(int x, int y, const Cell& cell);

    // Overwriting either half of a wide pair blanks the partner cell so no
    // orphaned halves survive.
    void damageWidePair(int x, int y);
    void scrollUpRegion(int count, int top, int bottom);
    void scrollDownRegion(int count, int top, int bottom);
    void newlineWithScroll();

    const Theme& theme;
    int cols = 0;
    int rowCount = 0;
    std::vector<Line> grid;
    std::vector<Line> altGrid;
    std::deque<Line> scrollback;
    CursorPos savedCursor;
    Pen savedPen;
    int scrollTop = 0;
    int scrollBottom = 0;
    bool wrapPending = false;
    char32_t lastPrinted = 0;
    std::vector<bool> tabStops;
    std::uint64_t changeVersion = 1;
};
} // namespace term
