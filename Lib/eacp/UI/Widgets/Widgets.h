#pragma once

#include "../Component/Component.h"

#include <optional>
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

    // A face of its own, for a heading or a caption. Left unset, the label draws
    // in whatever the host's is, which is what makes a screenful of them look
    // like one interface.
    void setFont(const Font& font);

    // The host's face at another size or weight -- what a heading actually is,
    // and what keeps a label from naming a family the host may not be using.
    void setFontSize(float pointSize);
    void setFontStyle(FontStyle style);

    void paint(Graphics& g) override;

private:
    std::string text;
    Color colour = defaultTheme().text;
    Justification justification = Justification::Left;

    // Empty until asked for. A label that never mentions a font has to draw in
    // the painter's, and it cannot know what that is before it is painting.
    std::optional<Font> font;
    std::optional<float> pointSize;
    std::optional<FontStyle> style;
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

// A box and a tick, with a caption beside it. What a list of options is made of,
// and the one widget whose whole state is a bool.
class Checkbox final : public Component
{
public:
    explicit Checkbox(std::string textToUse = {});

    void setText(std::string newText);
    const std::string& getText() const { return text; }

    void setChecked(bool shouldBeChecked, bool notify = false);
    bool isChecked() const { return checked; }

    void setAccentColour(const Color& colour);

    std::function<void(bool)> onChange = [](bool) {};

    void paint(Graphics& g) override;

    void mouseEnter(const MouseEvent&) override;
    void mouseExit(const MouseEvent&) override;
    void mouseUp(const MouseEvent& event) override;

    // Space and Return toggle it, which is what makes a form usable without the
    // pointer. Everything else is passed on.
    bool keyDown(const KeyEvent& event) override;

private:
    std::string text;
    Color accent = defaultTheme().accent;
    bool checked = false;
};

// A single line of editable text: a caret, a selection, and the keys that move
// them.
//
// Drawn by the component tier like everything else, rather than hosting a native
// text field over the tree. The difference shows the first time an editor is put
// inside something that scrolls -- a native view does not move with the content
// it is over -- and in what it costs: this is a rectangle, a run of glyphs and a
// caret in the same batch as the interface around it.
//
// Single line on purpose. Wrapping is a layout problem the tier has not needed
// yet, and it is a different widget rather than a flag on this one.
//
// What it does not do: input methods, right-to-left text, and undo. All three
// are real work rather than omissions to be filled in later, and a document
// editor should not be built out of this until they are done.
class TextEditor final : public Component
{
public:
    explicit TextEditor(std::string textToUse = {});

    // Setting the text moves the caret to the end and drops the selection, the
    // way handing someone a field with something already in it does.
    void setText(std::string newText, bool notify = false);
    const std::string& getText() const { return text; }

    // Shown in place of empty text, dimmed. Not part of the value: it is never
    // returned by getText and typing does not have to clear it.
    void setPlaceholder(std::string newPlaceholder);

    void setReadOnly(bool shouldBeReadOnly);
    bool isReadOnly() const { return readOnly; }

    void setFont(const Font& font);
    void setColour(const Color& colour);
    void setAccentColour(const Color& colour);

    // In bytes, clamped, and never inside a UTF-8 sequence -- so a caret can be
    // used as a substring boundary without splitting a character.
    void setCaretPosition(int position);
    int getCaretPosition() const { return caret; }

    void selectAll();
    void deselect();
    bool hasSelection() const { return selectionStart != caret; }
    std::string getSelectedText() const;

    std::function<void(const std::string&)> onTextChange = [](const std::string&) {};

    // Return, and the escape that means "put it back". A field that commits on
    // Return usually wants both, and neither is the same as losing focus.
    std::function<void(const std::string&)> onReturnKey = [](const std::string&) {};
    std::function<void()> onEscapeKey = [] {};

    void paint(Graphics& g) override;

    bool keyDown(const KeyEvent& event) override;

    void mouseDown(const MouseEvent& event) override;
    void mouseDrag(const MouseEvent& event) override;

    void focusGained() override;
    void focusLost() override;

private:
    Font fontToDrawIn() const;

    // Byte offsets, moved a whole UTF-8 sequence at a time.
    int nextCharacter(int from) const;
    int previousCharacter(int from) const;

    int selectionLeft() const { return std::min(selectionStart, caret); }
    int selectionRight() const { return std::max(selectionStart, caret); }

    void moveCaret(int position, bool extendSelection);
    void replaceSelection(const std::string& with);
    void deleteBackwards();
    void deleteForwards();

    // Which byte offset a click at `x` landed on, by walking characters until
    // the measured width passes it. Linear in the string, which for one line of
    // a field is nothing and would be the wrong shape for a document.
    int positionAt(float x) const;

    // Where the text starts drawing, once a long string has been scrolled to
    // keep the caret in view.
    float textOrigin() const;
    void scrollToCaret();

    bool handleClipboardKey(const KeyEvent& event);

    std::string text;
    std::string placeholder;

    Color colour = defaultTheme().text;
    Color accent = defaultTheme().accent;
    std::optional<Font> font;

    int caret = 0;

    // The other end of the selection. Equal to the caret means none, which is
    // why there is no separate flag to keep in step with it.
    int selectionStart = 0;

    float scrollOffset = 0.f;
    bool readOnly = false;
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
