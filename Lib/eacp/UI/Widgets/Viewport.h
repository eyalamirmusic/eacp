#pragma once

#include "Widgets.h"

#include <functional>

namespace eacp::UI
{
// How a Viewport draws its scrollbars.
struct ScrollbarStyle
{
    Color track = defaultTheme().outline;
    Color thumb = defaultTheme().dimText;

    // Across the bar. The strip a shown bar takes is kept clear of the content
    // the scroll can reach, so the last row or column is never under it.
    float thickness = 8.f;

    // Along the bar, so the thumb of a very long content stays something to grab.
    float minimumThumbLength = 24.f;
};

// A window onto a component larger than itself, scrolled on both axes.
//
// Where ScrollPanel is a vertical page that sizes its content's width to fit,
// this leaves the content exactly the size it was given and scrolls whichever
// axes it overflows: by the wheel, a finger (with a fling), or the scrollbars,
// whose thumbs drag and whose tracks page. It is transparent unless told to
// draw a background, so it can sit on whatever its parent paints.
//
// The view position is the content point shown at the viewport's top-left
// corner, always clamped to the content: an axis the content fits on does not
// scroll at all, and a wheel turned over content that fits is left for a
// parent to take.
class Viewport : public Component
{
public:
    Viewport();

    // The component to scroll. Not owned, and not resized: it keeps whatever
    // size it has. Replacing it removes the previous one and returns to the
    // top-left corner.
    void setContent(Component& newContent);
    Component* getContent() const;

    // Resizes the content while keeping the view where it is, as far as the
    // new size allows.
    void setContentSize(float width, float height);

    // Re-reads the content's size after something else changed it.
    void contentResized();

    void setViewPosition(Point newPosition);
    Point getViewPosition() const;

    // The furthest the view can go on each axis: zero on an axis the content
    // fits on.
    Point getMaximumViewPosition() const;

    // The part of the viewport the content shows through, which is the whole
    // of it less the strip of each scrollbar that is shown.
    Rect getViewArea() const;

    void setScrollbarsShown(bool horizontal, bool vertical);
    bool isHorizontalScrollbarShown() const;
    bool isVerticalScrollbarShown() const;

    void setScrollbarStyle(const ScrollbarStyle& newStyle);
    const ScrollbarStyle& getScrollbarStyle() const;

    // Where each bar's track and thumb are, in this component's coordinates.
    // Empty while that bar is not shown.
    Rect getHorizontalTrackBounds() const;
    Rect getVerticalTrackBounds() const;
    Rect getHorizontalThumbBounds() const;
    Rect getVerticalThumbBounds() const;

    void setDrawsBackground(bool shouldDraw);
    bool getDrawsBackground() const;

    // What one notch of a wheel that counts in lines scrolls, in points.
    void setWheelLineSize(float points);

    bool isFlinging() const;

    std::function<void(Point)> onViewPositionChanged = [](Point) {};

    void paint(Graphics& g) override;
    void paintOverChildren(Graphics& g) override;
    void resized() override;

    bool mouseWheelMove(const MouseEvent& event) override;

    void mouseDown(const MouseEvent& event) override;
    void mouseDrag(const MouseEvent& event) override;
    void mouseUp(const MouseEvent& event) override;
    void mouseCancel(const MouseEvent&) override;

    bool interceptsTouch(const MouseEvent& event) override;
    bool claimsTouchDrag(const MouseEvent& event) override;
    bool advanceAnimation(double seconds) override;

private:
    // A bar's track as a mouse target. Children are hit before their parent,
    // so without one on top of the content a press on a bar would land on
    // whatever the content has under it.
    class ScrollbarArea final : public Component
    {
    public:
        ScrollbarArea(Viewport& ownerToUse, ScrollAxes axisToUse);

        void mouseDown(const MouseEvent& event) override;
        void mouseDrag(const MouseEvent& event) override;
        void mouseUp(const MouseEvent& event) override;
        void mouseCancel(const MouseEvent& event) override;
        bool claimsTouchDrag(const MouseEvent& event) override;

    private:
        Point origin() const;

        Viewport& owner;
        ScrollAxes axis;
    };

    struct BarsShown
    {
        bool horizontal = false;
        bool vertical = false;
    };

    BarsShown barsShown() const;
    Point contentSize() const;
    Rect trackBounds(ScrollAxes axis) const;
    Rect thumbBounds(ScrollAxes axis) const;

    void layOut();
    void placeScrollbarArea(ScrollbarArea& area, const Rect& track);
    void moveTo(Point wanted);
    void raiseScrollbarAreas();

    void pressScrollbar(ScrollAxes axis, Point position);
    void dragScrollbar(ScrollAxes axis, Point position);
    void releaseScrollbar();

    Component* content = nullptr;
    Point viewPosition;

    ScrollbarStyle style;
    bool showHorizontalBar = true;
    bool showVerticalBar = true;
    bool drawsBackground = false;
    float wheelLineSize = 40.f;

    bool draggingThumb = false;
    ScrollAxes thumbAxis = ScrollAxes::vertical;
    float thumbDragStart = 0.f;
    float viewAtThumbDragStart = 0.f;

    ScrollbarArea horizontalArea {*this, ScrollAxes::horizontal};
    ScrollbarArea verticalArea {*this, ScrollAxes::vertical};

    TouchScrolling touchScroll {*this};
};
} // namespace eacp::UI
