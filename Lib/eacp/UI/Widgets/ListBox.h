#pragma once

#include "Widgets.h"

#include <string>

namespace eacp::UI
{
// Where a ListBox gets its rows.
//
// A model rather than a component per row, and that is the whole design: a log
// with ten thousand lines in it is one component, and what it costs to draw is
// the rows that are actually on screen. A row is not a thing that can be
// clicked, focused or laid out -- it is a rectangle the model paints into --
// which is the trade a list of that size has to make.
struct ListBoxModel
{
    virtual ~ListBoxModel() = default;

    virtual int getNumRows() = 0;

    // The row's background, and its content in a list with no columns.
    // `rowBounds` is in the ListBox's own coordinates, already scrolled, and the
    // painter is clipped to the area rows are drawn in.
    //
    // Drawing the selection is the model's, which is why it is told: a row is
    // whatever the model paints, and a highlight the list drew over or under it
    // would be a second opinion about what a selected row looks like.
    virtual void
        paintRow(Graphics& g, int row, const Rect& rowBounds, bool selected) = 0;

    // One cell, called for every column after paintRow has drawn the row it is
    // in. Never called in a list that has no columns.
    virtual void paintCell(Graphics&, int, int, const Rect&, bool) {}

    virtual void selectedRowChanged(int) {}
    virtual void rowDoubleClicked(int) {}
};

// A scrolling list of rows painted by a model, with an optional header that
// turns it into a table.
class ListBox final : public Component
{
public:
    // A table column: what the header says, and how wide the cell is.
    struct Column
    {
        std::string name;
        float width = 100.f;
    };

    ListBox();

    // Not owned, and has to outlive the list. Re-reads the row count, so a
    // model set after its data is ready needs no second call.
    void setModel(ListBoxModel* newModel);
    ListBoxModel* getModel() const { return model; }

    void setRowHeight(float newHeight);
    float getRowHeight() const { return rowHeight; }

    // Asks the model how many rows there are now, and puts the selection and
    // the scroll position back inside them. Silent: a selection dropped because
    // the row it named has gone is not the user changing it, and a model
    // rebuilding its data does not want to hear about the rows it destroyed.
    void updateContent();
    int getNumRows() const { return numRows; }

    // -1 means nothing is selected, and is the only value outside the list that
    // is accepted. Does not scroll -- scrollToRow is separate, so a selection
    // arriving from elsewhere cannot move a list somebody is reading.
    void setSelectedRow(int row, bool notify = false);
    int getSelectedRow() const { return selectedRow; }

    // As far as it has to be and no further, from whichever edge the row left:
    // a row already in view does not move the list at all.
    void scrollToRow(int row);

    void setScrollPosition(float newOffset);
    float getScrollPosition() const { return scrollOffset; }

    // Which row is at `localPosition`, or -1 for the header, the gap under the
    // last row, or outside.
    int getRowAt(Point localPosition) const;

    // Where a row is drawn, in this component's own coordinates. Answers for
    // any row, including one scrolled out of view.
    Rect getRowBounds(int row) const;

    // Everything under the header, which is where the rows go.
    Rect getRowArea() const;

    // Setting columns is what makes this a table: the header appears, and the
    // model is asked for cells as well as rows.
    void setColumns(Vector<Column> newColumns);
    const Vector<Column>& getColumns() const { return columns; }

    void setHeaderHeight(float newHeight);

    // Zero while there are no columns, there being no header to leave room for.
    float getHeaderHeight() const;

    // Where `column` sits inside `rowBounds` -- what the model splits a row
    // with, and what the list itself lays the header out with, so the two
    // cannot drift apart. Empty for a column that does not exist.
    Rect getColumnBounds(int column, const Rect& rowBounds) const;

    void paint(Graphics& g) override;

    void mouseDown(const MouseEvent& event) override;
    bool mouseWheelMove(const MouseEvent& event) override;

private:
    void paintHeader(Graphics& g);
    void paintRows(Graphics& g);
    void paintScrollIndicator(Graphics& g);

    float maximumScroll() const;

    ListBoxModel* model = nullptr;

    Vector<Column> columns;

    // Read from the model by updateContent rather than asked for while
    // painting: the count decides what the selection and the scroll may be, and
    // a list whose length changed under its own paint() would be drawing
    // against one answer and hit-testing against another.
    int numRows = 0;

    int selectedRow = -1;
    float rowHeight = 22.f;
    float headerHeight = 24.f;
    float scrollOffset = 0.f;
};
} // namespace eacp::UI
