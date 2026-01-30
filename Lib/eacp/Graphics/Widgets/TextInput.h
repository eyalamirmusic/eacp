#pragma once

#include <eacp/Core/Core.h>
#include "../View/View.h"
#include "../Layers/ShapeLayer.h"
#include "../Layers/TextLayer.h"

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
    void setTextColor(const Color& color);
    void setBackgroundColor(const Color& color);
    void setBorderColor(const Color& color);
    void setPadding(float padding);

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
    Color textColor {0.f, 0.f, 0.f};
    Color unfocusedBorderColor {0.6f, 0.6f, 0.6f};
    Color focusedBorderColor {0.2f, 0.5f, 1.f};

    ShapeLayer backgroundLayer;
    ShapeLayer borderLayer;
    TextLayer textLayer;
    ShapeLayer cursorLayer;

    std::function<void(const std::string&)> onChangeCallback;
    std::function<void(const std::string&)> onSubmitCallback;

    Threads::Timer cursorTimer {[this] { toggleCursorVisibility(); }, 2};
    bool cursorVisible = true;
};

} // namespace eacp::Graphics
