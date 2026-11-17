#pragma once

#include "View.h"

#include <functional>
#include <string>

namespace eacp::Graphics
{
// A text field the platform edits itself, for text a person composes rather
// than keys an app reads: on iOS a UITextView, so dictation, autocorrect, the
// space-bar cursor and correction by tapping a word all work. Elsewhere it is
// an empty view for now: nothing types into it.
class NativeTextView : public View
{
public:
    NativeTextView();
    ~NativeTextView() override;

    std::string getText() const;
    void setText(const std::string& text);

    void setFont(const std::string& family, float pointSize);
    void setColours(Color text, Color background);

    // Takes the keyboard's typing, bringing up the software keyboard, or gives
    // it up, putting the software keyboard away.
    void startEditing();
    void stopEditing();
    bool isEditing() const;

    // Each piece of text about to go in (typed, dictated or an autocorrection),
    // "\n" for the return key. Answer true to keep it out of the field: what a
    // composer does to send on return instead of starting a new line.
    std::function<bool(const std::string& incoming)> onIncomingText =
        [](const std::string&) { return false; };

    std::function<void()> onTextChanged = [] {};

    // A hardware keyboard's key going down while the field is editing, before
    // the field acts on it. Answer true to keep it from the field: what a
    // composer does with the up arrow to recall an earlier message. iOS only.
    std::function<bool(const KeyEvent& key)> onKeyDown = [](const KeyEvent&)
    { return false; };

    void resized() override;
    void* nativeFocusTarget() override;

    struct Native;

private:
    Pimpl<Native> native;
};
} // namespace eacp::Graphics
