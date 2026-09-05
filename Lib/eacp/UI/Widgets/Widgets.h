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

    // Whether the editor draws the panel and outline behind its text. On by
    // default; off for a field whose surroundings draw its frame -- a form
    // control on a styled page has its border and background from the page,
    // and the editor is then the text, the selection and the caret alone.
    void setDrawsFrame(bool shouldDrawFrame);
    bool getDrawsFrame() const { return drawsFrame; }

    // Drawn in place of every character, for a password field. The text is
    // kept and returned as typed; only the drawing, and the hit testing that
    // goes with it, use the mask. Empty -- the default -- draws the text.
    void setPasswordCharacter(std::string mask);
    const std::string& getPasswordCharacter() const { return passwordCharacter; }

    // In bytes, clamped, and never inside a UTF-8 sequence -- so a caret can be
    // used as a substring boundary without splitting a character.
    void setCaretPosition(int position);
    int getCaretPosition() const { return caret; }

    void selectAll();
    void deselect();
    bool hasSelection() const { return selectionStart != caret; }
    std::string getSelectedText() const;

    std::function<void(const std::string&)> onTextChange = [](const std::string&) {};

    // When the editor takes the keyboard and when it gives it up, for whoever
    // shows a field's focus somewhere other than in the editor's own frame.
    std::function<void(bool focused)> onFocusChange = [](bool) {};

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

    // What is drawn for the first `bytes` of the text: the text itself, or
    // one mask per character of it in a password field. Everything that
    // measures goes through here, so the caret and a click agree with what
    // is on screen whichever it is.
    std::string displayedPrefix(int bytes) const;
    std::string displayed() const { return displayedPrefix((int) text.size()); }

    std::string text;
    std::string placeholder;
    std::string passwordCharacter;

    Color colour = defaultTheme().text;
    Color accent = defaultTheme().accent;
    std::optional<Font> font;

    int caret = 0;

    // The other end of the selection. Equal to the caret means none, which is
    // why there is no separate flag to keep in step with it.
    int selectionStart = 0;

    float scrollOffset = 0.f;
    bool readOnly = false;
    bool drawsFrame = true;
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
    //
    // Silent unless asked, the way Checkbox::setChecked and TextEditor::setText
    // are, and for the reason a control attached to something else needs: a
    // value arriving from that something -- a parameter moved by automation, a
    // preset loaded -- must not come back out as a change and be written to it
    // again. The mouse paths ask.
    void setValue(float newValue, bool notify = false);
    float getValue() const { return value; }

    // Where a double-click puts the value. Unset by default, and then a second
    // click is an ordinary press: a control with no default has nowhere to go.
    void setDefaultValue(std::optional<float> newDefault);
    const std::optional<float>& getDefaultValue() const { return defaultValue; }

    void setAccentColour(const Color& colour);

    std::function<void(float)> onValueChange = [](float) {};

    // The two ends of a gesture, which is what a host recording automation
    // needs around the values it is given: exactly one onDragEnd for every
    // onDragStart, the double-click reset included -- it is a whole gesture of
    // its own rather than the start of a drag.
    std::function<void()> onDragStart = [] {};
    std::function<void()> onDragEnd = [] {};

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
    std::optional<float> defaultValue;
    Color accent = defaultTheme().accent;
    bool dragging = false;
};

// A rotary control, built the way JUCE's stock look builds one: a track arc,
// a value arc over it, and a round thumb where the value arc ends.
//
// Both arcs are strokes with round caps, so every end is a semicircle, and the
// thumb is a disc twice the stroke's width centred on the ring -- concentric
// with the cap it sits on, which is what makes the two read as one shape
// rather than as an arc with something stuck to it. Same angles, inset and
// line width as JUCE, so a knob laid out at the sizes its demos use comes out
// the same shape.
//
// The one stock widget whose shape a rounded rectangle cannot express, and so
// the one that shows what the path tier is for. Each arc is a PathShape --
// rasterized to exact per-pixel coverage by a compute kernel when its geometry
// changes, into the same atlas every other knob on screen uses, and drawn as
// one quad in the same instanced batch as the rectangles and glyphs around it.
// The track changes only with the size, and every knob of one size shares one
// mask of it; the thumb is a rounded rectangle from the distance field and
// costs no path at all. A hundred of them cost a few hundred quads, not a
// hundred draws.
//
// Dragged vertically rather than in a circle, which is what every rotary
// control that is any good to use does: the hand does not have to trace the
// shape it is turning.
class Knob final : public Component
{
public:
    Knob();

    // Normalised 0-1, clamped, and silent unless asked -- see Slider::setValue,
    // which this matches in every respect.
    void setValue(float newValue, bool notify = false);
    float getValue() const { return value; }

    void setDefaultValue(std::optional<float> newDefault);
    const std::optional<float>& getDefaultValue() const { return defaultValue; }

    void setAccentColour(const Color& colour);

    std::function<void(float)> onValueChange = [](float) {};

    std::function<void()> onDragStart = [] {};
    std::function<void()> onDragEnd = [] {};

    void paint(Graphics& g) override;
    void resized() override;

    void mouseEnter(const MouseEvent&) override;
    void mouseExit(const MouseEvent&) override;
    void mouseDown(const MouseEvent& event) override;
    void mouseDrag(const MouseEvent& event) override;
    void mouseUp(const MouseEvent&) override;

private:
    void rebuildTrack();
    void rebuildArc();

    float value = 0.5f;
    float valueAtDragStart = 0.5f;
    std::optional<float> defaultValue;
    Color accent = defaultTheme().accent;
    bool dragging = false;

    PathShape track {*this};
    PathShape arc {*this};
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
