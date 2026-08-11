#pragma once

#include "../Component/Component.h"

#include <optional>
#include <string>

namespace eacp::UI
{
// The colours the stock widgets draw with.
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

    // Left unset, the label draws in the host's face.
    void setFont(const Font& font);

    // The host's face at another size or weight.
    void setFontSize(float pointSize);
    void setFontStyle(FontStyle style);

    void paint(Graphics& g) override;

private:
    std::string text;
    Color colour = defaultTheme().text;
    Justification justification = Justification::Left;

    // Empty until asked for: without one, the painter's face is used.
    std::optional<Font> font;
    std::optional<float> pointSize;
    std::optional<FontStyle> style;
};

class Button final : public Component
{
public:
    explicit Button(std::string textToUse = {});

    void setText(std::string newText);

    // Latching rather than momentary. Off by default.
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

// A box and a tick, with a caption beside it.
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

    // Space and Return toggle it; everything else is passed on.
    bool keyDown(const KeyEvent& event) override;

private:
    std::string text;
    Color accent = defaultTheme().accent;
    bool checked = false;
};

// A single line of editable text: a caret, a selection, and the keys that move
// them. No input methods, no right-to-left text, and no undo.
class TextEditor final : public Component
{
public:
    explicit TextEditor(std::string textToUse = {});

    // Moves the caret to the end and drops the selection.
    void setText(std::string newText, bool notify = false);
    const std::string& getText() const { return text; }

    // Shown dimmed in place of empty text, and never part of the value.
    void setPlaceholder(std::string newPlaceholder);

    void setReadOnly(bool shouldBeReadOnly);
    bool isReadOnly() const { return readOnly; }

    void setFont(const Font& font);
    void setColour(const Color& colour);
    void setAccentColour(const Color& colour);

    // In bytes, clamped, and never inside a UTF-8 sequence.
    void setCaretPosition(int position);
    int getCaretPosition() const { return caret; }

    void selectAll();
    void deselect();
    bool hasSelection() const { return selectionStart != caret; }
    std::string getSelectedText() const;

    std::function<void(const std::string&)> onTextChange = [](const std::string&) {};

    // Neither is the same as losing focus.
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

    // The byte offset a click at `x` landed on. Linear in the string.
    int positionAt(float x) const;

    // Where the text starts drawing, once scrolled to keep the caret in view.
    float textOrigin() const;
    void scrollToCaret();

    bool handleClipboardKey(const KeyEvent& event);

    std::string text;
    std::string placeholder;

    Color colour = defaultTheme().text;
    Color accent = defaultTheme().accent;
    std::optional<Font> font;

    int caret = 0;

    // The other end of the selection; equal to the caret means none.
    int selectionStart = 0;

    float scrollOffset = 0.f;
    bool readOnly = false;
};

// A linear fader. Vertical runs bottom-to-top, unlike y elsewhere.
class Slider final : public Component
{
public:
    enum class Orientation
    {
        Horizontal,
        Vertical
    };

    explicit Slider(Orientation orientationToUse = Orientation::Horizontal);

    // Normalised 0-1, clamped.
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

// A rotary control: a ring, an arc filled to the value, and a pointer. Dragged
// vertically rather than in a circle.
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
class ScrollPanel final : public Component
{
public:
    ScrollPanel();

    // Not owned. Its width is set to the panel's; its height is the caller's.
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
