#pragma once

#include "View.h"
#include "ShapeLayer.h"
#include "TextLayer.h"
#include "../Primitives/Font.h"
#include <functional>
#include <string>

namespace eacp::Graphics
{

class TextInput : public View
{
public:
    TextInput(const FontOptions& options);
    TextInput(const std::string& initialText);

    void setText(const std::string& text);
    std::string getText() const;

    void setCursorPosition(size_t position);
    size_t getCursorPosition() const;

    void setPlaceholder(const std::string& placeholder);

    void setFont(const FontOptions& font);

    void onChange(std::function<void(const std::string&)> callback);
    void onSubmit(std::function<void(const std::string&)> callback);

    void resized() override;
    void keyDown(const KeyEvent& event) override;
    void mouseDown(const MouseEvent& event) override;

private:
    void initialize();
    void updateTextDisplay();
    void updateCursorPosition();
    void handleBackspace();
    void handleDelete();
    void toggleCursorVisibility();
    void updateBorderColor();

    std::string text;
    std::string placeholder;
    size_t cursorIndex = 0;
    Font font;
    float padding = 8.f;

    ShapeLayer backgroundLayer;
    ShapeLayer borderLayer;
    TextLayer textLayer;
    ShapeLayer cursorLayer;

    std::function<void(const std::string&)> onChangeCallback;
    std::function<void(const std::string&)> onSubmitCallback;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
