#include "Widgets.h"

#include <algorithm>

namespace eacp::UI
{
// Corner radii, in points. Kept together because what makes a set of controls
// look like a set is that they agree about this, and a literal at each call
// site is how they stop agreeing.
constexpr auto buttonCorner = 5.f;
constexpr auto trackCorner = 4.f;

const Theme& defaultTheme()
{
    static const auto theme = Theme {};
    return theme;
}

Label::Label(std::string textToUse)
    : text(std::move(textToUse))
{
}

void Label::setText(std::string newText)
{
    text = std::move(newText);
    repaint();
}

void Label::setColour(const Color& colourToUse)
{
    colour = colourToUse;
    repaint();
}

void Label::setJustification(Justification newJustification)
{
    justification = newJustification;
    repaint();
}

void Label::paint(Graphics& g)
{
    g.setColour(colour);
    g.drawText(text, getLocalBounds(), justification);
}

Button::Button(std::string textToUse)
    : text(std::move(textToUse))
{
    setInterceptsMouseClicks(true);
    setMouseCursor(eacp::Graphics::MouseCursor::PointingHand);
}

void Button::setText(std::string newText)
{
    text = std::move(newText);
    repaint();
}

void Button::setToggleable(bool shouldToggle)
{
    toggleable = shouldToggle;
    repaint();
}

void Button::setToggleState(bool shouldBeOn)
{
    if (toggledOn == shouldBeOn)
        return;

    toggledOn = shouldBeOn;
    repaint();
}

void Button::setAccentColour(const Color& colour)
{
    accent = colour;
    repaint();
}

void Button::paint(Graphics& g)
{
    const auto& theme = defaultTheme();
    auto bounds = getLocalBounds();

    g.setColour(toggledOn ? accent : theme.panel);
    g.fillRoundedRect(bounds, buttonCorner);

    if (down)
    {
        g.setColour(theme.pressed);
        g.fillRoundedRect(bounds, buttonCorner);
    }
    else if (isMouseOver())
    {
        g.setColour(theme.hover);
        g.fillRoundedRect(bounds, buttonCorner);
    }

    g.setColour(theme.outline);
    g.drawRoundedRect(bounds, buttonCorner);

    // A lit toggle carries a dark caption, so it stays legible against the
    // accent rather than sitting light-on-light.
    g.setColour(toggledOn ? Color {0.08f, 0.09f, 0.12f, 1.f} : theme.text);
    g.drawText(text, bounds, Justification::Centred);
}

void Button::mouseEnter(const MouseEvent&)
{
    repaint();
}

void Button::mouseExit(const MouseEvent&)
{
    down = false;
    repaint();
}

void Button::mouseDown(const MouseEvent&)
{
    down = true;
    repaint();
}

void Button::mouseUp(const MouseEvent& event)
{
    auto wasDown = down;
    down = false;

    // Only a release still inside the button counts, so a press dragged away
    // and let go is a cancel -- what every platform's buttons do.
    if (wasDown && hitTest(event.position))
    {
        if (toggleable)
            toggledOn = !toggledOn;

        onClick();
    }

    repaint();
}

Slider::Slider(Orientation orientationToUse)
    : orientation(orientationToUse)
{
    setInterceptsMouseClicks(true);
    setMouseCursor(orientationToUse == Orientation::Horizontal
                       ? eacp::Graphics::MouseCursor::ResizeLeftRight
                       : eacp::Graphics::MouseCursor::ResizeUpDown);
}

void Slider::setValue(float newValue)
{
    auto clamped = std::clamp(newValue, 0.f, 1.f);

    if (clamped == value)
        return;

    value = clamped;
    onValueChange(value);
    repaint();
}

void Slider::setAccentColour(const Color& colour)
{
    accent = colour;
    repaint();
}

void Slider::paint(Graphics& g)
{
    const auto& theme = defaultTheme();
    auto bounds = getLocalBounds();

    g.setColour(theme.panel);
    g.fillRoundedRect(bounds, trackCorner);

    if (orientation == Orientation::Horizontal)
    {
        g.setColour(accent);
        g.fillRoundedRect(bounds.withWidth(bounds.w * value), trackCorner);

        auto thumbX = bounds.w * value;
        g.setColour(theme.text);
        g.fillRoundedRect({thumbX - 1.5f, bounds.y + 2.f, 3.f, bounds.h - 4.f},
                          1.5f);
    }
    else
    {
        // Bottom-to-top: a fader's zero is at the bottom whatever way y runs.
        auto filledHeight = bounds.h * value;

        g.setColour(accent);
        g.fillRoundedRect(
            {bounds.x, bounds.bottom() - filledHeight, bounds.w, filledHeight},
            trackCorner);

        g.setColour(theme.text);
        g.fillRoundedRect({bounds.x + 2.f,
                           bounds.bottom() - filledHeight - 1.5f,
                           bounds.w - 4.f,
                           3.f},
                          1.5f);
    }

    g.setColour(isMouseOver() || dragging ? theme.accent : theme.outline);
    g.drawRoundedRect(bounds, trackCorner);
}

void Slider::setValueFromPosition(Point position)
{
    auto bounds = getLocalBounds();

    if (orientation == Orientation::Horizontal)
        setValue(bounds.w > 0.f ? position.x / bounds.w : 0.f);
    else
        setValue(bounds.h > 0.f ? 1.f - position.y / bounds.h : 0.f);
}

void Slider::mouseEnter(const MouseEvent&)
{
    repaint();
}

void Slider::mouseExit(const MouseEvent&)
{
    repaint();
}

void Slider::mouseDown(const MouseEvent& event)
{
    dragging = true;
    setValueFromPosition(event.position);
    repaint();
}

void Slider::mouseDrag(const MouseEvent& event)
{
    setValueFromPosition(event.position);
}

void Slider::mouseUp(const MouseEvent&)
{
    dragging = false;
    repaint();
}

ScrollPanel::ScrollPanel()
{
    setInterceptsMouseClicks(true);
}

void ScrollPanel::setContent(Component& newContent)
{
    content = &newContent;
    addAndMakeVisible(newContent);
    resized();
}

float ScrollPanel::maximumScroll() const
{
    if (content == nullptr)
        return 0.f;

    return std::max(0.f, content->getHeight() - getHeight());
}

void ScrollPanel::setScrollPosition(float newOffset)
{
    auto clamped = std::clamp(newOffset, 0.f, maximumScroll());

    if (clamped == scrollOffset)
        return;

    scrollOffset = clamped;
    resized();
    repaint();
}

void ScrollPanel::resized()
{
    if (content == nullptr)
        return;

    scrollOffset = std::clamp(scrollOffset, 0.f, maximumScroll());

    content->setBounds({0.f, -scrollOffset, getWidth(), content->getHeight()});
}

void ScrollPanel::paint(Graphics& g)
{
    g.fillAll(defaultTheme().background);
}

void ScrollPanel::paintOverChildren(Graphics& g)
{
    auto maximum = maximumScroll();

    if (maximum <= 0.f)
        return;

    const auto& theme = defaultTheme();
    auto bounds = getLocalBounds();
    auto trackWidth = 4.f;
    auto track = bounds.fromRight(trackWidth);

    auto visibleProportion = getHeight() / content->getHeight();
    auto thumbHeight = std::max(24.f, bounds.h * visibleProportion);
    auto thumbY = (bounds.h - thumbHeight) * (scrollOffset / maximum);

    g.setColour(theme.outline);
    g.fillRoundedRect(track, trackWidth * 0.5f);

    g.setColour(theme.dimText);
    g.fillRoundedRect({track.x, thumbY, trackWidth, thumbHeight}, trackWidth * 0.5f);
}

bool ScrollPanel::mouseWheelMove(const MouseEvent& event)
{
    if (maximumScroll() <= 0.f)
        return false;

    // A trackpad reports points and is applied as it comes; a notched wheel
    // reports lines, and only this component knows what a line is worth here.
    auto step = event.preciseWheel ? event.wheelDelta.y : event.wheelDelta.y * 40.f;

    setScrollPosition(scrollOffset - step);
    return true;
}
} // namespace eacp::UI
