#include "Widgets.h"

#include <eacp/Core/App/Clipboard.h>

#include <algorithm>

namespace eacp::UI
{
// Corner radii, in points. Kept together because what makes a set of controls
// look like a set is that they agree about this, and a literal at each call
// site is how they stop agreeing.
constexpr auto buttonCorner = 5.f;
constexpr auto trackCorner = 4.f;

// The gap between an editor's border and the first glyph. Also what the caret
// has to stay clear of, which is why it is a constant rather than a literal in
// the scrolling arithmetic.
constexpr auto textInset = 6.f;

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

void Label::setFont(const Font& fontToUse)
{
    font = fontToUse;
    repaint();
}

void Label::setFontSize(float pointSizeToUse)
{
    pointSize = pointSizeToUse;
    repaint();
}

void Label::setFontStyle(FontStyle styleToUse)
{
    style = styleToUse;
    repaint();
}

void Label::paint(Graphics& g)
{
    if (font.has_value())
        g.setFont(*font);

    // After the whole face, so a label given both keeps the size it was told
    // last -- and so setFontSize on its own means "the host's, bigger".
    if (pointSize.has_value())
        g.setFontSize(*pointSize);

    if (style.has_value())
        g.setFontStyle(*style);

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

namespace
{
// Whether this is the platform's "do the editing shortcut" modifier: Command on
// Apple, Control everywhere else. Both are accepted rather than one chosen by
// #if, so a Windows keyboard on a Mac and a Mac keyboard on Windows both work.
bool isShortcutModifier(const ModifierKeys& modifiers)
{
    return modifiers.command || modifiers.control;
}

bool isContinuationByte(char byte)
{
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}
} // namespace

Checkbox::Checkbox(std::string textToUse)
    : text(std::move(textToUse))
{
    setInterceptsMouseClicks(true);
    setWantsKeyboardFocus(true);
    setMouseCursor(eacp::Graphics::MouseCursor::PointingHand);
}

void Checkbox::setText(std::string newText)
{
    text = std::move(newText);
    repaint();
}

void Checkbox::setChecked(bool shouldBeChecked, bool notify)
{
    if (checked == shouldBeChecked)
        return;

    checked = shouldBeChecked;
    repaint();

    if (notify)
        onChange(checked);
}

void Checkbox::setAccentColour(const Color& colour)
{
    accent = colour;
    repaint();
}

void Checkbox::mouseEnter(const MouseEvent&)
{
    repaint();
}

void Checkbox::mouseExit(const MouseEvent&)
{
    repaint();
}

void Checkbox::mouseUp(const MouseEvent& event)
{
    // Only a release inside counts, so a press dragged off the box is a change
    // of mind rather than a toggle.
    if (getLocalBounds().contains(event.position))
        setChecked(!checked, true);
}

bool Checkbox::keyDown(const KeyEvent& event)
{
    if (event.keyCode != eacp::Graphics::KeyCode::Space
        && event.keyCode != eacp::Graphics::KeyCode::Return)
        return false;

    setChecked(!checked, true);

    return true;
}

void Checkbox::paint(Graphics& g)
{
    const auto& theme = defaultTheme();
    auto bounds = getLocalBounds();

    auto boxSize = std::min(18.f, bounds.h);
    auto box = Rect {0.f, (bounds.h - boxSize) * 0.5f, boxSize, boxSize};

    g.setColour(checked ? accent : theme.panel);
    g.fillRoundedRect(box, 4.f);

    g.setColour(isMouseOver() ? theme.text : theme.outline);
    g.drawRoundedRect(box, 4.f, hasKeyboardFocus() ? 2.f : 1.f);

    // A tick out of two strokes rather than a path: two lines are two quads in
    // the batch the rest of the tree is drawing in, where a path would be a
    // mask in the atlas for every checkbox on screen.
    if (checked)
    {
        g.setColour(theme.background);

        auto inset = boxSize * 0.26f;
        auto left = Point {box.x + inset, box.y + boxSize * 0.52f};
        auto middle = Point {box.x + boxSize * 0.42f, box.bottom() - inset};
        auto right = Point {box.right() - inset, box.y + inset};

        g.drawLine(left, middle, 2.f);
        g.drawLine(middle, right, 2.f);
    }

    if (text.empty())
        return;

    auto textArea = bounds;
    textArea.x = box.right() + 8.f;
    textArea.w -= textArea.x;

    g.setColour(theme.text);
    g.drawText(text, textArea);
}

TextEditor::TextEditor(std::string textToUse)
    : text(std::move(textToUse))
{
    setInterceptsMouseClicks(true);
    setWantsKeyboardFocus(true);
    setMouseCursor(eacp::Graphics::MouseCursor::IBeam);

    caret = (int) text.size();
    selectionStart = caret;
}

Font TextEditor::fontToDrawIn() const
{
    return font.has_value() ? *font : getHostFont();
}

void TextEditor::setText(std::string newText, bool notify)
{
    text = std::move(newText);

    caret = (int) text.size();
    selectionStart = caret;
    scrollOffset = 0.f;

    repaint();

    if (notify)
        onTextChange(text);
}

void TextEditor::setPlaceholder(std::string newPlaceholder)
{
    placeholder = std::move(newPlaceholder);
    repaint();
}

void TextEditor::setReadOnly(bool shouldBeReadOnly)
{
    readOnly = shouldBeReadOnly;
    repaint();
}

void TextEditor::setFont(const Font& fontToUse)
{
    font = fontToUse;
    repaint();
}

void TextEditor::setColour(const Color& colourToUse)
{
    colour = colourToUse;
    repaint();
}

void TextEditor::setAccentColour(const Color& colourToUse)
{
    accent = colourToUse;
    repaint();
}

void TextEditor::setDrawsFrame(bool shouldDrawFrame)
{
    drawsFrame = shouldDrawFrame;
    repaint();
}

void TextEditor::setPasswordCharacter(std::string mask)
{
    passwordCharacter = std::move(mask);
    scrollToCaret();
    repaint();
}

std::string TextEditor::displayedPrefix(int bytes) const
{
    auto prefix = std::string_view {text}.substr(0, (std::size_t) bytes);

    if (passwordCharacter.empty())
        return std::string {prefix};

    auto masked = std::string {};

    for (auto c: prefix)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
            masked += passwordCharacter;

    return masked;
}

int TextEditor::nextCharacter(int from) const
{
    auto position = std::clamp(from, 0, (int) text.size());

    if (position >= (int) text.size())
        return (int) text.size();

    ++position;

    while (position < (int) text.size()
           && isContinuationByte(text[(std::size_t) position]))
        ++position;

    return position;
}

int TextEditor::previousCharacter(int from) const
{
    auto position = std::clamp(from, 0, (int) text.size());

    if (position <= 0)
        return 0;

    --position;

    while (position > 0 && isContinuationByte(text[(std::size_t) position]))
        --position;

    return position;
}

void TextEditor::setCaretPosition(int position)
{
    moveCaret(position, false);
}

void TextEditor::moveCaret(int position, bool extendSelection)
{
    caret = std::clamp(position, 0, (int) text.size());

    // Never left inside a UTF-8 sequence, so the caret is always a safe
    // substring boundary for the measuring and the editing both.
    while (caret > 0 && caret < (int) text.size()
           && isContinuationByte(text[(std::size_t) caret]))
        --caret;

    if (!extendSelection)
        selectionStart = caret;

    scrollToCaret();
    repaint();
}

void TextEditor::selectAll()
{
    selectionStart = 0;
    caret = (int) text.size();

    scrollToCaret();
    repaint();
}

void TextEditor::deselect()
{
    selectionStart = caret;
    repaint();
}

std::string TextEditor::getSelectedText() const
{
    if (!hasSelection())
        return {};

    return text.substr((std::size_t) selectionLeft(),
                       (std::size_t) (selectionRight() - selectionLeft()));
}

void TextEditor::replaceSelection(const std::string& with)
{
    if (readOnly)
        return;

    auto left = selectionLeft();
    auto right = selectionRight();

    text.replace((std::size_t) left, (std::size_t) (right - left), with);

    caret = left + (int) with.size();
    selectionStart = caret;

    scrollToCaret();
    repaint();
    onTextChange(text);
}

void TextEditor::deleteBackwards()
{
    if (readOnly)
        return;

    if (!hasSelection())
    {
        if (caret == 0)
            return;

        selectionStart = previousCharacter(caret);
    }

    replaceSelection({});
}

void TextEditor::deleteForwards()
{
    if (readOnly)
        return;

    if (!hasSelection())
    {
        if (caret >= (int) text.size())
            return;

        selectionStart = nextCharacter(caret);
    }

    replaceSelection({});
}

float TextEditor::textOrigin() const
{
    return textInset - scrollOffset;
}

void TextEditor::scrollToCaret()
{
    auto visible = getWidth() - textInset * 2.f;

    if (visible <= 0.f)
        return;

    auto caretX = measureText(displayedPrefix(caret), fontToDrawIn());

    // Only ever as far as it has to be: the caret is pulled back into view from
    // whichever edge it left, and a string that fits is never scrolled at all.
    scrollOffset = std::clamp(scrollOffset, caretX - visible, caretX);
    scrollOffset = std::max(0.f, scrollOffset);

    auto full = measureText(displayed(), fontToDrawIn());

    if (full - scrollOffset < visible)
        scrollOffset = std::max(0.f, full - visible);
}

int TextEditor::positionAt(float x) const
{
    auto target = x - textOrigin();

    if (target <= 0.f)
        return 0;

    auto face = fontToDrawIn();
    auto position = 0;

    while (position < (int) text.size())
    {
        auto next = nextCharacter(position);

        auto before = measureText(displayedPrefix(position), face);
        auto after = measureText(displayedPrefix(next), face);

        // Past the middle of a character means the one after it, which is what
        // makes clicking the right-hand half of a letter put the caret behind
        // it rather than in front.
        if (target < (before + after) * 0.5f)
            return position;

        position = next;
    }

    return (int) text.size();
}

void TextEditor::mouseDown(const MouseEvent& event)
{
    grabKeyboardFocus();

    auto position = positionAt(event.position.x);

    // Shift-click extends rather than replaces, the way a second click with the
    // modifier held means "to here" in every text field.
    moveCaret(position, event.modifiers.shift);
}

void TextEditor::mouseDrag(const MouseEvent& event)
{
    moveCaret(positionAt(event.position.x), true);
}

void TextEditor::focusGained()
{
    repaint();
    onFocusChange(true);
}

void TextEditor::focusLost()
{
    deselect();
    repaint();
    onFocusChange(false);
}

bool TextEditor::handleClipboardKey(const KeyEvent& event)
{
    if (!isShortcutModifier(event.modifiers))
        return false;

    const auto& pressed = event.charactersIgnoringModifiers;

    if (pressed == "a")
    {
        selectAll();
        return true;
    }

    if (pressed == "c" || pressed == "x")
    {
        if (hasSelection())
            Clipboard::copyText(getSelectedText());

        if (pressed == "x" && !readOnly)
            replaceSelection({});

        return true;
    }

    if (pressed == "v")
    {
        if (!readOnly && Clipboard::hasText())
            replaceSelection(Clipboard::getText());

        return true;
    }

    return false;
}

bool TextEditor::keyDown(const KeyEvent& event)
{
    using namespace eacp::Graphics;

    if (handleClipboardKey(event))
        return true;

    // Anything else with the shortcut modifier held belongs to the window, not
    // to this field: an editor that swallowed Cmd+S would be the reason a
    // document could not be saved while a field had focus.
    if (isShortcutModifier(event.modifiers))
        return false;

    const auto extend = event.modifiers.shift;

    switch (event.keyCode)
    {
        case KeyCode::LeftArrow:
            moveCaret(previousCharacter(caret), extend);
            return true;

        case KeyCode::RightArrow:
            moveCaret(nextCharacter(caret), extend);
            return true;

        case KeyCode::Home:
        case KeyCode::UpArrow:
            moveCaret(0, extend);
            return true;

        case KeyCode::End:
        case KeyCode::DownArrow:
            moveCaret((int) text.size(), extend);
            return true;

        case KeyCode::Delete:
            deleteBackwards();
            return true;

        case KeyCode::ForwardDelete:
            deleteForwards();
            return true;

        case KeyCode::Return:
        case KeyCode::KeypadEnter:
            onReturnKey(text);
            return true;

        case KeyCode::Escape:
            onEscapeKey();
            return true;

        default:
            break;
    }

    // Typing, which is anything that produced a printable character. Control
    // characters are excluded rather than listed: Tab has to reach the traversal
    // above this, and a key that produced nothing has nothing to insert.
    if (readOnly || event.characters.empty())
        return false;

    if (static_cast<unsigned char>(event.characters.front()) < 0x20)
        return false;

    replaceSelection(event.characters);

    return true;
}

void TextEditor::paint(Graphics& g)
{
    const auto& theme = defaultTheme();
    auto bounds = getLocalBounds();
    auto focused = hasKeyboardFocus();

    if (drawsFrame)
    {
        g.setColour(theme.panel);
        g.fillRoundedRect(bounds, 4.f);

        g.setColour(focused ? accent : theme.outline);
        g.drawRoundedRect(bounds, 4.f, focused ? 2.f : 1.f);
    }

    auto face = fontToDrawIn();
    g.setFont(face);

    // Everything below is drawn against the text's own origin, which a long
    // string has scrolled. Clipped to the inside of the border so a scrolled
    // string is cut at the edge rather than running over it.
    auto scope = Graphics::ScopedState {g};
    g.reduceClipRegion(bounds.inset(textInset * 0.5f, 1.f));

    auto baseline = (bounds.h - g.lineHeight()) * 0.5f + g.ascent();
    auto origin = textOrigin();

    if (text.empty() && !placeholder.empty())
    {
        g.setColour(theme.dimText);
        g.drawText(placeholder, {origin, baseline});
        return;
    }

    if (hasSelection() && focused)
    {
        auto left = g.measureText(displayedPrefix(selectionLeft()));
        auto right = g.measureText(displayedPrefix(selectionRight()));

        g.setColour(accent.withAlpha(0.35f));
        g.fillRect({origin + left, 2.f, right - left, bounds.h - 4.f});
    }

    g.setColour(colour);
    g.drawText(displayed(), {origin, baseline});

    if (!focused || readOnly)
        return;

    // Always on rather than blinking. A blink is a timer per editor and a
    // repaint of the tree twice a second for as long as a field has focus,
    // which is a real cost in a tier whose whole claim is that it draws nothing
    // while nothing changes.
    auto caretX = origin + g.measureText(displayedPrefix(caret));

    g.setColour(accent);
    g.fillRect({caretX, 3.f, 1.5f, bounds.h - 6.f});
}

Slider::Slider(Orientation orientationToUse)
    : orientation(orientationToUse)
{
    setInterceptsMouseClicks(true);
    setMouseCursor(orientationToUse == Orientation::Horizontal
                       ? eacp::Graphics::MouseCursor::ResizeLeftRight
                       : eacp::Graphics::MouseCursor::ResizeUpDown);
}

void Slider::setValue(float newValue, bool notify)
{
    auto clamped = std::clamp(newValue, 0.f, 1.f);

    if (clamped == value)
        return;

    value = clamped;
    repaint();

    if (notify)
        onValueChange(value);
}

void Slider::setDefaultValue(std::optional<float> newDefault)
{
    defaultValue = newDefault;
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
        setValue(bounds.w > 0.f ? position.x / bounds.w : 0.f, true);
    else
        setValue(bounds.h > 0.f ? 1.f - position.y / bounds.h : 0.f, true);
}

namespace
{
// A double-click on a control that has a default is a whole gesture on its own:
// bracketed like a drag, so a host recording automation writes one value and
// closes the recording, and followed by no drag at all -- which is why the
// caller stops here rather than carrying on into its press.
//
// Written once against both controls rather than twice: they differ in how a
// drag reaches a value and in nothing about how a gesture is shaped.
template <typename Control>
bool resetToDefault(Control& control, const MouseEvent& event)
{
    if (event.clickCount < 2 || !control.getDefaultValue().has_value())
        return false;

    control.onDragStart();
    control.setValue(*control.getDefaultValue(), true);
    control.onDragEnd();

    return true;
}
} // namespace

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
    if (resetToDefault(*this, event))
        return;

    dragging = true;
    onDragStart();
    setValueFromPosition(event.position);
    repaint();
}

void Slider::mouseDrag(const MouseEvent& event)
{
    if (dragging)
        setValueFromPosition(event.position);
}

// The release that ends the press this widget started, and only that one: the
// host captures a press, so the mouseUp after a double-click reset arrives here
// too and would otherwise close a gesture that was already closed.
void Slider::mouseUp(const MouseEvent&)
{
    if (!dragging)
        return;

    dragging = false;
    onDragEnd();
    repaint();
}

namespace
{
// Where the arc starts and how far it sweeps: the usual rotary gap at the
// bottom, so full and empty are visibly different positions rather than the
// same one. Measured clockwise from straight up, in the y-down space paths are
// authored in.
constexpr auto knobStartAngle = -0.75f * GPUWidgets::pi;
constexpr auto knobSweepAngle = 1.5f * GPUWidgets::pi;

// Points per unit of drag. A full sweep in about two hundred pixels of travel,
// which is fine enough to set a value and coarse enough to reach both ends.
constexpr auto knobDragRange = 200.f;

Point onCircle(Point centre, float radius, float angle)
{
    return {centre.x + std::sin(angle) * radius,
            centre.y - std::cos(angle) * radius};
}

// A ring segment as a closed contour: out along the start radius, round the
// outside, back down the end radius, and round the inside the other way.
void addArc(GPUWidgets::Path& path,
            Point centre,
            float innerRadius,
            float outerRadius,
            float fromAngle,
            float toAngle)
{
    auto sweep = toAngle - fromAngle;

    if (sweep <= 0.f || outerRadius <= innerRadius)
        return;

    // Enough steps that the flattening tolerance decides the smoothness rather
    // than this does - Path subdivides curves adaptively, but an arc built out
    // of lineTo has only what it is given.
    auto steps = std::max(8, (int) std::ceil(sweep * outerRadius * 0.5f));

    path.moveTo(onCircle(centre, outerRadius, fromAngle));

    for (auto i = 1; i <= steps; ++i)
        path.lineTo(onCircle(
            centre, outerRadius, fromAngle + sweep * (float) i / (float) steps));

    for (auto i = steps; i >= 0; --i)
        path.lineTo(onCircle(
            centre, innerRadius, fromAngle + sweep * (float) i / (float) steps));

    path.close();
}
} // namespace

Knob::Knob()
{
    setInterceptsMouseClicks(true);
    setMouseCursor(eacp::Graphics::MouseCursor::ResizeUpDown);
}

void Knob::setValue(float newValue, bool notify)
{
    auto clamped = std::clamp(newValue, 0.f, 1.f);

    if (clamped == value)
        return;

    value = clamped;
    rebuildIndicator();
    repaint();

    if (notify)
        onValueChange(value);
}

void Knob::setDefaultValue(std::optional<float> newDefault)
{
    defaultValue = newDefault;
}

void Knob::setAccentColour(const Color& colour)
{
    accent = colour;
    repaint();
}

void Knob::resized()
{
    rebuildIndicator();
}

// Built here rather than in paint(), because rasterizing coverage is a compute
// dispatch and a compute pass cannot be opened inside the render pass paint()
// draws into. Called whenever the geometry changes -- which is what the value
// changing means for a rotary control -- so the mask the next frame samples is
// always the current one. See PathShape.
void Knob::rebuildIndicator()
{
    auto bounds = getLocalBounds();
    auto size = std::min(bounds.w, bounds.h);

    if (size <= 0.f)
    {
        indicator.clear();
        return;
    }

    auto centre = Point {bounds.w * 0.5f, bounds.h * 0.5f};
    auto outer = size * 0.5f - 1.f;
    auto thickness = std::max(2.f, size * 0.12f);

    auto path = GPUWidgets::Path {};

    addArc(path,
           centre,
           outer - thickness,
           outer,
           knobStartAngle,
           knobStartAngle + knobSweepAngle * value);

    // The pointer, as a second contour of the same path: one shape, one mask,
    // one quad -- and no seam where it crosses the arc, since coverage
    // accumulates within a path rather than between two draws of one.
    //
    // Wound the same way round as the arc, and that is load-bearing rather than
    // tidy: under the non-zero rule two contours that disagree *subtract* where
    // they overlap, so the wrong order here punches a hole through the join
    // instead of merging with it.
    auto pointerWidth = std::max(1.5f, size * 0.045f);
    auto angle = knobStartAngle + knobSweepAngle * value;
    auto tip = onCircle(centre, outer - thickness * 0.5f, angle);
    auto across =
        Point {std::cos(angle) * pointerWidth, std::sin(angle) * pointerWidth};

    path.moveTo({centre.x - across.x, centre.y - across.y});
    path.lineTo({tip.x - across.x, tip.y - across.y});
    path.lineTo({tip.x + across.x, tip.y + across.y});
    path.lineTo({centre.x + across.x, centre.y + across.y});
    path.close();

    indicator.setPath(path);
}

void Knob::paint(Graphics& g)
{
    const auto& theme = defaultTheme();
    auto bounds = getLocalBounds();
    auto size = std::min(bounds.w, bounds.h);
    auto centre = Point {bounds.w * 0.5f, bounds.h * 0.5f};
    auto outer = size * 0.5f - 1.f;

    // The unfilled track is a ring, which the distance field draws as a
    // bordered circle for nothing -- so only the part that actually needs a
    // path becomes one.
    auto thickness = std::max(2.f, size * 0.12f);

    g.setColour(theme.outline);
    g.drawRoundedRect({centre.x - outer, centre.y - outer, outer * 2.f, outer * 2.f},
                      outer,
                      thickness);

    g.setColour(isMouseOver() || dragging ? theme.text : accent);
    g.fillPath(indicator);
}

void Knob::mouseEnter(const MouseEvent&)
{
    repaint();
}

void Knob::mouseExit(const MouseEvent&)
{
    repaint();
}

void Knob::mouseDown(const MouseEvent& event)
{
    if (resetToDefault(*this, event))
        return;

    dragging = true;
    valueAtDragStart = value;
    onDragStart();
    repaint();
}

// Against where the drag started rather than against the last event, so the
// value cannot drift from accumulated rounding over a long drag.
void Knob::mouseDrag(const MouseEvent& event)
{
    if (!dragging)
        return;

    auto travel = event.downPosition.y - event.position.y;
    setValue(valueAtDragStart + travel / knobDragRange, true);
}

void Knob::mouseUp(const MouseEvent&)
{
    if (!dragging)
        return;

    dragging = false;
    onDragEnd();
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
