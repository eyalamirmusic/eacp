#include "NativeTextView.h"

// Only iOS has a native text field behind this so far; elsewhere the view
// holds its text and draws nothing.
namespace eacp::Graphics
{
struct NativeTextView::Native
{
    std::string text;
};

NativeTextView::NativeTextView() = default;
NativeTextView::~NativeTextView() = default;

std::string NativeTextView::getText() const
{
    return native->text;
}

void NativeTextView::setText(const std::string& text)
{
    native->text = text;
}

void NativeTextView::setFont(const std::string&, float) {}
void NativeTextView::setColours(Color, Color) {}
void NativeTextView::startEditing() {}
void NativeTextView::stopEditing() {}

bool NativeTextView::isEditing() const
{
    return false;
}

void NativeTextView::resized() {}

void* NativeTextView::nativeFocusTarget()
{
    return getHandle();
}
} // namespace eacp::Graphics
