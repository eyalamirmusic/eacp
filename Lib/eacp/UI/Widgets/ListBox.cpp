#include "ListBox.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
namespace
{
// What a header caption is indented by. A model is free to indent its cells
// differently, but the stock header and a stock cell agreeing is what makes a
// table look like columns rather than like two lists.
constexpr auto headerInset = 6.f;

constexpr auto indicatorWidth = 4.f;
} // namespace

ListBox::ListBox()
{
    setInterceptsMouseClicks(true);
}

void ListBox::setModel(ListBoxModel* newModel)
{
    model = newModel;
    updateContent();
}

void ListBox::setRowHeight(float newHeight)
{
    rowHeight = std::max(1.f, newHeight);

    scrollOffset = std::clamp(scrollOffset, 0.f, maximumScroll());
    repaint();
}

void ListBox::setHeaderHeight(float newHeight)
{
    headerHeight = std::max(0.f, newHeight);

    scrollOffset = std::clamp(scrollOffset, 0.f, maximumScroll());
    repaint();
}

float ListBox::getHeaderHeight() const
{
    return columns.empty() ? 0.f : headerHeight;
}

void ListBox::setColumns(Vector<Column> newColumns)
{
    columns = std::move(newColumns);

    scrollOffset = std::clamp(scrollOffset, 0.f, maximumScroll());
    repaint();
}

void ListBox::updateContent()
{
    numRows = model != nullptr ? model->getNumRows() : 0;

    // Clamped rather than kept: both name rows, and the rows they named may
    // have gone. min() with the last index is also what clears the selection
    // when the list is now empty.
    selectedRow = std::min(selectedRow, numRows - 1);
    scrollOffset = std::clamp(scrollOffset, 0.f, maximumScroll());

    repaint();
}

void ListBox::setSelectedRow(int row, bool notify)
{
    if (row < -1 || row >= numRows || row == selectedRow)
        return;

    selectedRow = row;
    repaint();

    if (notify && model != nullptr)
        model->selectedRowChanged(selectedRow);
}

float ListBox::maximumScroll() const
{
    return std::max(0.f, (float) numRows * rowHeight - getRowArea().h);
}

void ListBox::setScrollPosition(float newOffset)
{
    auto clamped = std::clamp(newOffset, 0.f, maximumScroll());

    if (clamped == scrollOffset)
        return;

    scrollOffset = clamped;
    repaint();
}

void ListBox::scrollToRow(int row)
{
    if (row < 0 || row >= numRows)
        return;

    auto top = (float) row * rowHeight;
    auto lowest = top + rowHeight - getRowArea().h;

    // A row taller than the area showing it can only be aligned to its top,
    // which is also what keeps the clamp below from being asked for a range
    // that runs backwards.
    setScrollPosition(lowest > top ? top : std::clamp(scrollOffset, lowest, top));
}

Rect ListBox::getRowArea() const
{
    auto area = getLocalBounds();
    area.removeFromTop(getHeaderHeight());

    return area;
}

Rect ListBox::getRowBounds(int row) const
{
    auto rows = getRowArea();

    return {
        rows.x, rows.y + (float) row * rowHeight - scrollOffset, rows.w, rowHeight};
}

int ListBox::getRowAt(Point localPosition) const
{
    auto rows = getRowArea();

    if (rowHeight <= 0.f || !rows.contains(localPosition))
        return -1;

    auto row =
        (int) std::floor((localPosition.y - rows.y + scrollOffset) / rowHeight);

    return row >= 0 && row < numRows ? row : -1;
}

Rect ListBox::getColumnBounds(int column, const Rect& rowBounds) const
{
    if (column < 0 || column >= columns.size())
        return {};

    auto x = rowBounds.x;

    for (auto index = 0; index < column; ++index)
        x += columns[index].width;

    return {x, rowBounds.y, columns[column].width, rowBounds.h};
}

void ListBox::mouseDown(const MouseEvent& event)
{
    auto row = getRowAt(event.position);

    if (row < 0 || model == nullptr)
        return;

    setSelectedRow(row, true);

    if (event.clickCount >= 2)
        model->rowDoubleClicked(row);
}

bool ListBox::mouseWheelMove(const MouseEvent& event)
{
    if (maximumScroll() <= 0.f)
        return false;

    // Points from a trackpad, lines from a notched wheel -- the same conversion
    // ScrollPanel makes, and only this component knows what a line is worth in
    // it.
    auto step = event.preciseWheel ? event.wheelDelta.y : event.wheelDelta.y * 40.f;

    setScrollPosition(scrollOffset - step);

    return true;
}

void ListBox::paintHeader(Graphics& g)
{
    auto height = getHeaderHeight();

    if (height <= 0.f)
        return;

    const auto& theme = defaultTheme();
    auto header = getLocalBounds().withHeight(height);

    g.setColour(theme.panel);
    g.fillRect(header);

    g.setColour(theme.dimText);

    for (auto index = 0; index < columns.size(); ++index)
        g.drawText(columns[index].name,
                   getColumnBounds(index, header).inset(headerInset, 0.f));

    g.setColour(theme.outline);
    g.drawLine({header.x, header.bottom()}, {header.right(), header.bottom()});
}

void ListBox::paintRows(Graphics& g)
{
    if (model == nullptr || rowHeight <= 0.f)
        return;

    auto rows = getRowArea();

    auto scope = Graphics::ScopedState {g};
    g.reduceClipRegion(rows);

    // Only the rows in the window. What makes a list of ten thousand cost what
    // a list of twenty costs, and the reason the count is read once rather than
    // asked of the model per row.
    auto first = std::max(0, (int) std::floor(scrollOffset / rowHeight));
    auto last =
        std::min(numRows - 1, (int) std::floor((scrollOffset + rows.h) / rowHeight));

    for (auto row = first; row <= last; ++row)
    {
        auto bounds = getRowBounds(row);
        auto selected = row == selectedRow;

        model->paintRow(g, row, bounds, selected);

        for (auto column = 0; column < columns.size(); ++column)
            model->paintCell(
                g, row, column, getColumnBounds(column, bounds), selected);
    }
}

void ListBox::paintScrollIndicator(Graphics& g)
{
    auto maximum = maximumScroll();

    if (maximum <= 0.f)
        return;

    const auto& theme = defaultTheme();
    auto rows = getRowArea();
    auto track = rows.fromRight(indicatorWidth);

    auto visibleProportion = rows.h / ((float) numRows * rowHeight);
    auto thumbHeight = std::max(24.f, rows.h * visibleProportion);
    auto thumbY = rows.y + (rows.h - thumbHeight) * (scrollOffset / maximum);

    g.setColour(theme.outline);
    g.fillRoundedRect(track, indicatorWidth * 0.5f);

    g.setColour(theme.dimText);
    g.fillRoundedRect({track.x, thumbY, indicatorWidth, thumbHeight},
                      indicatorWidth * 0.5f);
}

void ListBox::paint(Graphics& g)
{
    g.fillAll(defaultTheme().background);

    paintHeader(g);
    paintRows(g);
    paintScrollIndicator(g);
}
} // namespace eacp::UI
