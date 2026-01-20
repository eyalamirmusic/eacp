// Windows implementation of TextLayer using DirectWrite
#include "TextLayer.h"

#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

// Forward declaration of factory access
namespace eacp::Graphics
{
IDWriteFactory* getDWriteFactory();
}

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

struct TextLayer::Native
{
    Native()
    {
        // Create default text format
        auto* factory = getDWriteFactory();
        if (factory)
        {
            factory->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us",
                                      textFormat.GetAddressOf());
        }
    }

    std::wstring text;
    ComPtr<IDWriteTextFormat> textFormat;
    Color color {1.0f, 1.0f, 1.0f, 1.0f};

    Rect bounds;
    Point position;
    float opacity = 1.0f;
    bool hidden = false;
};

TextLayer::TextLayer()
    : impl()
{
}

void TextLayer::setText(const std::string& text)
{
    // Convert to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    impl->text.resize(len - 1); // Don't include null terminator in string
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, impl->text.data(), len);
}

void TextLayer::setFont(const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    if (textFormat)
    {
        // Copy the text format reference
        impl->textFormat = textFormat;
    }
}

void TextLayer::setColor(const Color& color)
{
    impl->color = color;
}

void* TextLayer::getNativeLayer()
{
    return &impl;
}

} // namespace eacp::Graphics
