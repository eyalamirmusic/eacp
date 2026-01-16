#include "TextInput.h"
#include "../Primitives/TextMetrics.h"
#include "../Primitives/Path.h"
#include <eacp/Core/Threads/Timer.h>
#include <memory>

namespace eacp::Graphics
{

struct TextInput::Native
{
    Native(TextInput* owner)
        : cursorTimer([owner]() { owner->toggleCursorVisibility(); }, 2)
    {
    }

    Threads::Timer cursorTimer;
    bool cursorVisible = true;
};

TextInput::TextInput(const FontOptions& options)
    : font(options)
    , impl(this)
{
    initialize();
}

TextInput::TextInput(const std::string& initialText)
    : text(initialText)
    , cursorIndex(initialText.length())
    , font()
    , impl(this)
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

    float lineHeight = TextMetrics::getLineHeight(font);
    float textY = (bounds.h - lineHeight) / 2.f;

    textLayer.setBounds({padding, textY, bounds.w - padding * 2.f, lineHeight});

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

    impl->cursorVisible = true;
    cursorLayer.setHidden(false);
}

void TextInput::mouseDown(const MouseEvent& event)
{
    float xOffset = event.pos.x - padding;

    if (xOffset < 0.f)
        xOffset = 0.f;

    cursorIndex = TextMetrics::getIndexForOffset(text, xOffset, font);
    updateCursorPosition();

    impl->cursorVisible = true;
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
        textLayer.setColor({0.f, 0.f, 0.f});
    }
}

void TextInput::updateCursorPosition()
{
    Rect bounds = getLocalBounds();
    float xOffset = TextMetrics::getOffsetForIndex(text, cursorIndex, font);
    float lineHeight = TextMetrics::getLineHeight(font);
    float textY = (bounds.h - lineHeight) / 2.f;

    Path cursorPath;
    cursorPath.addRect({padding + xOffset, textY, 2.f, lineHeight});
    cursorLayer.setPath(cursorPath);
    cursorLayer.setBounds(bounds);
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

    impl->cursorVisible = !impl->cursorVisible;
    cursorLayer.setHidden(!impl->cursorVisible);
}

void TextInput::updateBorderColor()
{
    if (hasFocus())
        borderLayer.setStrokeColor({0.2f, 0.5f, 1.f});
    else
        borderLayer.setStrokeColor({0.6f, 0.6f, 0.6f});
}

} // namespace eacp::Graphics
