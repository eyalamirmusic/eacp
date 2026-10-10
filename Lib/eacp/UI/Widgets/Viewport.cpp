#include "Viewport.h"

#include <algorithm>

namespace eacp::UI
{
namespace
{
bool isHorizontal(ScrollAxes axis)
{
    return axis == ScrollAxes::horizontal;
}

float along(Point point, ScrollAxes axis)
{
    if (isHorizontal(axis))
        return point.x;

    return point.y;
}

Point withAlong(Point point, ScrollAxes axis, float value)
{
    if (isHorizontal(axis))
        return {value, point.y};

    return {point.x, value};
}

float startAlong(const Rect& rect, ScrollAxes axis)
{
    if (isHorizontal(axis))
        return rect.x;

    return rect.y;
}

float lengthAlong(const Rect& rect, ScrollAxes axis)
{
    if (isHorizontal(axis))
        return rect.w;

    return rect.h;
}

bool samePoint(Point a, Point b)
{
    return a.x == b.x && a.y == b.y;
}
} // namespace

Viewport::ScrollbarArea::ScrollbarArea(Viewport& ownerToUse, ScrollAxes axisToUse)
    : owner(ownerToUse)
    , axis(axisToUse)
{
    setInterceptsMouseClicks(true);
}

void Viewport::ScrollbarArea::mouseDown(const MouseEvent& event)
{
    owner.pressScrollbar(axis, event.position + origin());
}

void Viewport::ScrollbarArea::mouseDrag(const MouseEvent& event)
{
    owner.dragScrollbar(axis, event.position + origin());
}

void Viewport::ScrollbarArea::mouseUp(const MouseEvent&)
{
    owner.releaseScrollbar();
}

void Viewport::ScrollbarArea::mouseCancel(const MouseEvent&)
{
    owner.releaseScrollbar();
}

Point Viewport::ScrollbarArea::origin() const
{
    return {getBounds().x, getBounds().y};
}

bool Viewport::ScrollbarArea::claimsTouchDrag(const MouseEvent&)
{
    return owner.draggingThumb;
}

Viewport::Viewport()
{
    setInterceptsMouseClicks(true);
    addChildComponent(horizontalArea);
    addChildComponent(verticalArea);
}

void Viewport::setContent(Component& newContent)
{
    if (content == &newContent)
        return;

    if (content != nullptr)
        removeChildComponent(*content);

    touchScroll.stop();
    content = &newContent;
    addAndMakeVisible(newContent);
    raiseScrollbarAreas();

    moveTo({});
}

Component* Viewport::getContent() const
{
    return content;
}

void Viewport::setContentSize(float width, float height)
{
    if (content == nullptr)
        return;

    content->setBounds(content->getBounds().withSize(width, height));
    contentResized();
}

void Viewport::contentResized()
{
    moveTo(viewPosition);
}

void Viewport::setViewPosition(Point newPosition)
{
    moveTo(newPosition);
}

Point Viewport::getViewPosition() const
{
    return viewPosition;
}

Point Viewport::contentSize() const
{
    if (content == nullptr)
        return {};

    return {content->getWidth(), content->getHeight()};
}

Viewport::BarsShown Viewport::barsShown() const
{
    auto size = contentSize();
    auto thickness = style.thickness;

    // A bar shown on one axis takes room from the other, which can make that
    // one overflow too; the second pass settles it, the answer only growing.
    auto vertical = showVerticalBar && size.y > getHeight();
    auto horizontal =
        showHorizontalBar && size.x > getWidth() - (vertical ? thickness : 0.f);
    vertical =
        showVerticalBar && size.y > getHeight() - (horizontal ? thickness : 0.f);

    return {horizontal, vertical};
}

Rect Viewport::getViewArea() const
{
    auto shown = barsShown();
    auto width = getWidth();
    auto height = getHeight();

    if (shown.vertical)
        width -= style.thickness;

    if (shown.horizontal)
        height -= style.thickness;

    return {0.f, 0.f, std::max(0.f, width), std::max(0.f, height)};
}

Point Viewport::getMaximumViewPosition() const
{
    auto size = contentSize();
    auto area = getViewArea();

    return {std::max(0.f, size.x - area.w), std::max(0.f, size.y - area.h)};
}

Rect Viewport::trackBounds(ScrollAxes axis) const
{
    auto shown = barsShown();
    auto area = getViewArea();
    auto thickness = style.thickness;

    if (isHorizontal(axis))
    {
        if (!shown.horizontal)
            return {};

        return {0.f, getHeight() - thickness, area.w, thickness};
    }

    if (!shown.vertical)
        return {};

    return {getWidth() - thickness, 0.f, thickness, area.h};
}

Rect Viewport::thumbBounds(ScrollAxes axis) const
{
    auto track = trackBounds(axis);

    if (track.isEmpty())
        return {};

    auto trackLength = lengthAlong(track, axis);
    auto visible = lengthAlong(getViewArea(), axis);
    auto total = along(contentSize(), axis);
    auto maximum = along(getMaximumViewPosition(), axis);

    auto shortest = std::min(style.minimumThumbLength, trackLength);
    auto length = std::clamp(trackLength * visible / total, shortest, trackLength);
    auto offset = 0.f;

    if (maximum > 0.f)
        offset = (trackLength - length) * along(viewPosition, axis) / maximum;

    if (isHorizontal(axis))
        return {track.x + offset, track.y, length, track.h};

    return {track.x, track.y + offset, track.w, length};
}

Rect Viewport::getHorizontalTrackBounds() const
{
    return trackBounds(ScrollAxes::horizontal);
}

Rect Viewport::getVerticalTrackBounds() const
{
    return trackBounds(ScrollAxes::vertical);
}

Rect Viewport::getHorizontalThumbBounds() const
{
    return thumbBounds(ScrollAxes::horizontal);
}

Rect Viewport::getVerticalThumbBounds() const
{
    return thumbBounds(ScrollAxes::vertical);
}

void Viewport::setScrollbarsShown(bool horizontal, bool vertical)
{
    showHorizontalBar = horizontal;
    showVerticalBar = vertical;
    moveTo(viewPosition);
}

bool Viewport::isHorizontalScrollbarShown() const
{
    return showHorizontalBar;
}

bool Viewport::isVerticalScrollbarShown() const
{
    return showVerticalBar;
}

void Viewport::setScrollbarStyle(const ScrollbarStyle& newStyle)
{
    style = newStyle;
    moveTo(viewPosition);
}

const ScrollbarStyle& Viewport::getScrollbarStyle() const
{
    return style;
}

void Viewport::setDrawsBackground(bool shouldDraw)
{
    if (drawsBackground == shouldDraw)
        return;

    drawsBackground = shouldDraw;
    repaint();
}

bool Viewport::getDrawsBackground() const
{
    return drawsBackground;
}

void Viewport::setWheelLineSize(float points)
{
    wheelLineSize = points;
}

bool Viewport::isFlinging() const
{
    return touchScroll.isFlinging();
}

void Viewport::moveTo(Point wanted)
{
    auto previous = viewPosition;

    viewPosition = wanted;
    layOut();

    if (!samePoint(previous, viewPosition))
        onViewPositionChanged(viewPosition);
}

void Viewport::layOut()
{
    auto maximum = getMaximumViewPosition();

    viewPosition = {std::clamp(viewPosition.x, 0.f, maximum.x),
                    std::clamp(viewPosition.y, 0.f, maximum.y)};

    if (content != nullptr)
    {
        auto size = contentSize();
        content->setBounds({-viewPosition.x, -viewPosition.y, size.x, size.y});
    }

    placeScrollbarArea(horizontalArea, getHorizontalTrackBounds());
    placeScrollbarArea(verticalArea, getVerticalTrackBounds());

    repaint();
}

void Viewport::placeScrollbarArea(ScrollbarArea& area, const Rect& track)
{
    area.setVisible(!track.isEmpty());

    if (!track.isEmpty())
        area.setBounds(track);
}

void Viewport::raiseScrollbarAreas()
{
    horizontalArea.toFront();
    verticalArea.toFront();
}

void Viewport::resized()
{
    moveTo(viewPosition);
}

void Viewport::paint(Graphics& g)
{
    if (drawsBackground)
        g.fillAll(defaultTheme().background);
}

void Viewport::paintOverChildren(Graphics& g)
{
    auto radius = style.thickness * 0.5f;

    for (auto axis: {ScrollAxes::horizontal, ScrollAxes::vertical})
    {
        auto track = trackBounds(axis);

        if (track.isEmpty())
            continue;

        g.setColour(style.track);
        g.fillRoundedRect(track, radius);

        g.setColour(style.thumb);
        g.fillRoundedRect(thumbBounds(axis), radius);
    }
}

bool Viewport::mouseWheelMove(const MouseEvent& event)
{
    auto maximum = getMaximumViewPosition();
    auto delta = event.wheelDelta;

    // A wheel that only turns one way scrolls across when shift is held, or
    // when across is the only way there is to go.
    auto onlyAcross = maximum.y <= 0.f && maximum.x > 0.f;

    if (delta.x == 0.f && (event.modifiers.shift || onlyAcross))
        delta = {delta.y, 0.f};

    // A trackpad reports points and is applied as it comes; a notched wheel
    // reports lines, and only this component knows what a line is worth here.
    if (!event.preciseWheel)
        delta = {delta.x * wheelLineSize, delta.y * wheelLineSize};

    auto movesAcross = maximum.x > 0.f && delta.x != 0.f;
    auto movesUpAndDown = maximum.y > 0.f && delta.y != 0.f;

    if (!movesAcross && !movesUpAndDown)
        return false;

    if (!movesAcross)
        delta.x = 0.f;

    if (!movesUpAndDown)
        delta.y = 0.f;

    touchScroll.stop();
    moveTo(viewPosition - delta);
    return true;
}

void Viewport::mouseDown(const MouseEvent& event)
{
    if (!event.fromTouch)
        return;

    touchScroll.setAxes(TouchScrolling::axesWithRoom(getMaximumViewPosition()));
    touchScroll.press(event);
}

void Viewport::mouseDrag(const MouseEvent& event)
{
    if (event.fromTouch)
        moveTo(touchScroll.drag(event, viewPosition));
}

void Viewport::mouseUp(const MouseEvent& event)
{
    if (event.fromTouch)
        touchScroll.release(event);
}

void Viewport::mouseCancel(const MouseEvent&)
{
    touchScroll.stop();
}

bool Viewport::interceptsTouch(const MouseEvent& event)
{
    return touchScroll.isFlinging()
           || TouchScrolling::isScrollDrag(event, getMaximumViewPosition());
}

bool Viewport::claimsTouchDrag(const MouseEvent& event)
{
    return interceptsTouch(event);
}

bool Viewport::advanceAnimation(double seconds)
{
    moveTo(touchScroll.advance(seconds, viewPosition, getMaximumViewPosition()));

    return touchScroll.isFlinging();
}

void Viewport::pressScrollbar(ScrollAxes axis, Point position)
{
    auto thumb = thumbBounds(axis);

    if (thumb.contains(position))
    {
        draggingThumb = true;
        thumbAxis = axis;
        thumbDragStart = along(position, axis);
        viewAtThumbDragStart = along(viewPosition, axis);
        return;
    }

    auto page = lengthAlong(getViewArea(), axis);

    if (along(position, axis) < startAlong(thumb, axis))
        page = -page;

    touchScroll.stop();
    moveTo(withAlong(viewPosition, axis, along(viewPosition, axis) + page));
}

void Viewport::dragScrollbar(ScrollAxes axis, Point position)
{
    if (!draggingThumb || axis != thumbAxis)
        return;

    auto travel =
        lengthAlong(trackBounds(axis), axis) - lengthAlong(thumbBounds(axis), axis);

    if (travel <= 0.f)
        return;

    auto maximum = along(getMaximumViewPosition(), axis);
    auto moved = along(position, axis) - thumbDragStart;

    moveTo(withAlong(
        viewPosition, axis, viewAtThumbDragStart + moved * maximum / travel));
}

void Viewport::releaseScrollbar()
{
    draggingThumb = false;
}
} // namespace eacp::UI
