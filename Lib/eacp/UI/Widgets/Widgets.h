#pragma once

#include "../Component/Component.h"

#include <string>

namespace eacp::UI
{
// The colours the stock widgets draw with. One struct rather than a setter per
// colour on each widget, so a theme is a value that can be shared, copied and
// swapped rather than a hundred assignments -- the job JUCE gives LookAndFeel,
// at the scale this prototype needs.
struct Theme
{
    Color background {0.11f, 0.12f, 0.15f, 1.f};
    Color panel {0.16f, 0.17f, 0.21f, 1.f};
    Color outline {1.f, 1.f, 1.f, 0.10f};
    Color text {0.92f, 0.93f, 0.96f, 1.f};
    Color dimText {0.60f, 0.63f, 0.70f, 1.f};
    Color accent {0.35f, 0.62f, 0.95f, 1.f};
    Color accentDim {0.35f, 0.62f, 0.95f, 0.35f};
    Color hover {1.f, 1.f, 1.f, 0.07f};
    Color pressed {1.f, 1.f, 1.f, 0.14f};
};

const Theme& defaultTheme();

class Label final : public Component
{
public:
    explicit Label(std::string textToUse = {});

    void setText(std::string newText);
    const std::string& getText() const { return text; }

    void setColour(const Color& colour);
    void setJustification(Justification newJustification);

    void paint(Graphics& g) override;

private:
    std::string text;
    Color colour = defaultTheme().text;
    Justification justification = Justification::Left;
};

class Button final : public Component
{
public:
    explicit Button(std::string textToUse = {});

    void setText(std::string newText);

    // Latching: the button keeps its state between clicks and draws it, the way
    // a mute or solo does. Off by default, when it is a momentary press.
    void setToggleable(bool shouldToggle);
    void setToggleState(bool shouldBeOn);
    bool getToggleState() const { return toggledOn; }

    void setAccentColour(const Color& colour);

    std::function<void()> onClick = [] {};

    void paint(Graphics& g) override;

    void mouseEnter(const MouseEvent&) override;
    void mouseExit(const MouseEvent&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseUp(const MouseEvent& event) override;

private:
    std::string text;
    Color accent = defaultTheme().accent;
    bool toggleable = false;
    bool toggledOn = false;
    bool down = false;
};

// A linear fader, horizontal or vertical. Vertical runs bottom-to-top, which is
// what a level control has to do however y is measured elsewhere.
class Slider final : public Component
{
public:
    enum class Orientation
    {
        Horizontal,
        Vertical
    };

    explicit Slider(Orientation orientationToUse = Orientation::Horizontal);

    // Normalised 0-1. Clamped, so a caller doing its own arithmetic cannot push
    // the thumb off the track.
    void setValue(float newValue);
    float getValue() const { return value; }

    void setAccentColour(const Color& colour);

    std::function<void(float)> onValueChange = [](float) {};

    void paint(Graphics& g) override;

    void mouseEnter(const MouseEvent&) override;
    void mouseExit(const MouseEvent&) override;
    void mouseDown(const MouseEvent& event) override;
    void mouseDrag(const MouseEvent& event) override;
    void mouseUp(const MouseEvent&) override;

private:
    void setValueFromPosition(Point position);

    Orientation orientation;
    float value = 0.5f;
    Color accent = defaultTheme().accent;
    bool dragging = false;
};

// A rotary control: a ring, an arc filled to the value, and a pointer.
//
// The one stock widget whose shape a rounded rectangle cannot express, and so
// the one that shows what the path tier is for. The arc and the pointer are a
// single PathShape -- rasterized to exact per-pixel coverage by a compute
// kernel whenever the value changes, into the same atlas every other knob on
// screen uses, and drawn as one quad in the same instanced batch as the
// rectangles and glyphs around it. A hundred of them cost a hundred quads, not
// a hundred draws.
//
// Dragged vertically rather than in a circle, which is what every rotary
// control that is any good to use does: the hand does not have to trace the
// shape it is turning.
class Knob final : public Component
{
public:
    Knob();

    // Normalised 0-1, clamped.
    void setValue(float newValue);
    float getValue() const { return value; }

    void setAccentColour(const Color& colour);

    std::function<void(float)> onValueChange = [](float) {};

    void paint(Graphics& g) override;
    void resized() override;

    void mouseEnter(const MouseEvent&) override;
    void mouseExit(const MouseEvent&) override;
    void mouseDown(const MouseEvent& event) override;
    void mouseDrag(const MouseEvent& event) override;
    void mouseUp(const MouseEvent&) override;

private:
    void rebuildIndicator();

    float value = 0.5f;
    float valueAtDragStart = 0.5f;
    Color accent = defaultTheme().accent;
    bool dragging = false;

    PathShape indicator {*this};
};

// A clipping viewport over a taller content component, scrolled by the wheel.
//
// The clipping is not this component's code: paint() gives every component a
// Graphics already clipped to its own bounds, so content taller than the
// viewport is cut at the edge without the viewport asking. What it adds is the
// scroll offset, the wheel handling and the position indicator.
class ScrollPanel final : public Component
{
public:
    ScrollPanel();

    // The component to scroll. Not owned; its width is set to the panel's, and
    // its height is whatever the caller made it.
    void setContent(Component& newContent);

    void setScrollPosition(float newOffset);
    float getScrollPosition() const { return scrollOffset; }

    void paint(Graphics& g) override;
    void paintOverChildren(Graphics& g) override;
    void resized() override;
    bool mouseWheelMove(const MouseEvent& event) override;

private:
    float maximumScroll() const;

    Component* content = nullptr;
    float scrollOffset = 0.f;
};
} // namespace eacp::UI
