#include "TextInput.h"
#include "../Primitives/TextMetrics.h"
#include "../Primitives/Path.h"

namespace eacp::Graphics
{

TextInput::TextInput(const FontOptions& options)
    : font(options)
{
    initialize();
}

TextInput::TextInput(const std::string& initialText)
    : text(initialText)
    , cursorIndex(initialText.length())
{
    initialize();
}

void TextInput::initialize()
{
    getProperties().handlesMouseEvents = true;
    getProperties().grabsFocusOnMouseDown = true;

    addLayer(backgroundLayer);
    addLayer(borderLayer);
    addLayer(textLayer);
    addLayer(cursorLayer);

    backgroundLayer.setFillColor({1.f, 1.f, 1.f});

    borderLayer.setFillColor({0.f, 0.f, 0.f, 0.f});
    borderLayer.setStrokeColor({0.6f, 0.6f, 0.6f});
    borderLayer.setStrokeWidth(1.f);

    textLayer.setFont(font);
    textLayer.setColor({0.f, 0.f, 0.f});

    cursorLayer.setFillColor({0.f, 0.f, 0.f});

    updateTextDisplay();
}

void TextInput::setText(const std::string& newText)
{
    text = newText;
    if (cursorIndex > text.length())
        cursorIndex = text.length();

    updateTextDisplay();
    updateCursorPosition();
}

std::string TextInput::getText() const
{
    return text;
}

void TextInput::setCursorPosition(size_t position)
{
    cursorIndex = std::min(position, text.length());
    updateCursorPosition();
}

size_t TextInput::getCursorPosition() const
{
    return cursorIndex;
}

void TextInput::setPlaceholder(const std::string& newPlaceholder)
{
    placeholder = newPlaceholder;
    updateTextDisplay();
}

void TextInput::setFont(const FontOptions& newFont)
{
    font.setFont(newFont);
    textLayer.setFont(font);
    updateCursorPosition();
}

void TextInput::setTextColor(const Color& color)
{
    textColor = color;
    cursorLayer.setFillColor(color);
    updateTextDisplay();
}

void TextInput::setBackgroundColor(const Color& color)
{
    backgroundLayer.setFillColor(color);
}

void TextInput::setBorderColor(const Color& color)
{
    unfocusedBorderColor = color;
    focusedBorderColor = color;
    updateBorderColor();
}

void TextInput::setPadding(float newPadding)
{
    padding = newPadding;
    resized();
}

void TextInput::onChange(std::function<void(const std::string&)> callback)
{
    onChangeCallback = std::move(callback);
}

void TextInput::onSubmit(std::function<void(const std::string&)> callback)
{
    onSubmitCallback = std::move(callback);
}

void TextInput::resized()
{
    Rect bounds = getLocalBounds();

    Path bgPath;
    bgPath.addRoundedRect(bounds, 4.f);
    backgroundLayer.setPath(bgPath);
    backgroundLayer.setBounds(bounds);

    Path borderPath;
    borderPath.addRoundedRect(bounds, 4.f);
    borderLayer.setPath(borderPath);
    borderLayer.setBounds(bounds);

    textLayer.setBounds(bounds);
    textLayer.setPosition({padding, 0.f});

    cursorLayer.setBounds(bounds);

    updateCursorPosition();
    updateBorderColor();
}

void TextInput::keyDown(const KeyEvent& event)
{
    if (event.modifiers.command)
    {
        if (event.keyCode == KeyCode::A)
        {
            cursorIndex = text.length();
            updateCursorPosition();
        }
        return;
    }

    if (event.keyCode == KeyCode::Delete)
    {
        handleBackspace();
    }
    else if (event.keyCode == KeyCode::LeftArrow)
    {
        if (cursorIndex > 0)
        {
            cursorIndex--;
            updateCursorPosition();
        }
    }
    else if (event.keyCode == KeyCode::RightArrow)
    {
        if (cursorIndex < text.length())
        {
            cursorIndex++;
            updateCursorPosition();
        }
    }
    else if (event.keyCode == KeyCode::Return)
    {
        if (onSubmitCallback)
            onSubmitCallback(text);
    }
    else if (!event.characters.empty())
    {
        char c = event.characters[0];
        if (c >= 32 && c < 127)
        {
            text.insert(cursorIndex, event.characters);
            cursorIndex += event.characters.length();
            updateTextDisplay();
            updateCursorPosition();

            if (onChangeCallback)
                onChangeCallback(text);
        }
    }

    cursorVisible = true;
    cursorLayer.setHidden(false);
}

void TextInput::mouseDown(const MouseEvent& event)
{
    float xOffset = event.pos.x - padding;

    if (xOffset < 0.f)
        xOffset = 0.f;

    cursorIndex = TextMetrics::getIndexForOffset(text, xOffset, font);
    updateCursorPosition();

    cursorVisible = true;
    cursorLayer.setHidden(false);
}

void TextInput::updateTextDisplay()
{
    if (text.empty() && !placeholder.empty())
    {
        textLayer.setText(placeholder);
        textLayer.setColor({0.6f, 0.6f, 0.6f});
    }
    else
    {
        textLayer.setText(text);
        textLayer.setColor(textColor);
    }
}

void TextInput::updateCursorPosition()
{
    float xOffset = TextMetrics::getOffsetForIndex(text, cursorIndex, font);
    float ascent = TextMetrics::getAscent(font);
    float descent = TextMetrics::getDescent(font);
    float textHeight = ascent + descent;

    Path cursorPath;
    cursorPath.addRect({0.f, 0.f, 2.f, textHeight});
    cursorLayer.setPath(cursorPath);
    cursorLayer.setPosition({padding + xOffset, 0.f});
}

void TextInput::handleBackspace()
{
    if (cursorIndex > 0 && !text.empty())
    {
        text.erase(cursorIndex - 1, 1);
        cursorIndex--;
        updateTextDisplay();
        updateCursorPosition();

        if (onChangeCallback)
            onChangeCallback(text);
    }
}

void TextInput::handleDelete()
{
    if (cursorIndex < text.length())
    {
        text.erase(cursorIndex, 1);
        updateTextDisplay();
        updateCursorPosition();

        if (onChangeCallback)
            onChangeCallback(text);
    }
}

void TextInput::toggleCursorVisibility()
{
    if (!hasFocus())
    {
        cursorLayer.setHidden(true);
        return;
    }

    cursorVisible = !cursorVisible;
    cursorLayer.setHidden(!cursorVisible);
}

void TextInput::updateBorderColor()
{
    if (hasFocus())
        borderLayer.setStrokeColor(focusedBorderColor);
    else
        borderLayer.setStrokeColor(unfocusedBorderColor);
}

} // namespace eacp::Graphics
