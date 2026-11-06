#pragma once

/** @file A single-line text field built from layers. EACP_HAS_CONTEXT only (Apple
    and Windows). */

#include "../View/View.h"
#include "../Layers/ShapeLayer.h"
#include "../Layers/TextLayer.h"

namespace eacp::Graphics
{

// A single-line editable field with a blinking cursor. For component trees, use
// the UI module's text editing instead.
class TextInput : public View
{
public:
    TextInput(const FontOptions& options);
    TextInput(const std::string& initialText);

    void setText(const std::string& text);
    std::string getText() const;

    void setCursorPosition(int position);
    int getCursorPosition() const;

    void setPlaceholder(const std::string& placeholder);

    void setFont(const FontOptions& font);
    void setTextColor(const Color& color);
    void setBackgroundColor(const Color& color);
    void setBorderColor(const Color& color);
    void setPadding(float padding);

    // Called on the main thread after every edit, with the whole text.
    void onChange(std::function<void(const std::string&)> callback);
    // Called on the main thread when Return is pressed.
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
    int cursorIndex = 0;
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
