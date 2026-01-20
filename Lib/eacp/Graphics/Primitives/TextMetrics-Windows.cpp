// Windows implementation of TextMetrics using DirectWrite
#include "TextMetrics.h"

#include <cassert>

#define NOMINMAX
#include <Windows.h>
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

float TextMetrics::measureWidth(const std::string& text, const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    auto* factory = getDWriteFactory();

    if (!textFormat || !factory)
        return 0.0f;

    // Convert text to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wideText(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wideText.data(), len);

    // Create text layout for measurement
    ComPtr<IDWriteTextLayout> textLayout;
    factory->CreateTextLayout(wideText.c_str(), static_cast<UINT32>(wideText.length()),
                              textFormat, 10000.0f, 10000.0f, textLayout.GetAddressOf());

    if (!textLayout)
        return 0.0f;

    DWRITE_TEXT_METRICS metrics;
    textLayout->GetMetrics(&metrics);

    return metrics.width;
}

float TextMetrics::getOffsetForIndex(const std::string& text, size_t index,
                                     const Font& font)
{
    if (index == 0)
        return 0.0f;

    std::string substring = text.substr(0, index);
    return measureWidth(substring, font);
}

size_t TextMetrics::getIndexForOffset(const std::string& text, float xOffset,
                                      const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    auto* factory = getDWriteFactory();

    if (!textFormat || !factory)
        return 0;

    // Convert text to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wideText(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wideText.data(), len);

    // Create text layout
    ComPtr<IDWriteTextLayout> textLayout;
    factory->CreateTextLayout(wideText.c_str(), static_cast<UINT32>(wideText.length()),
                              textFormat, 10000.0f, 10000.0f, textLayout.GetAddressOf());

    if (!textLayout)
        return 0;

    // Use hit testing to find character at offset
    BOOL isTrailingHit = FALSE;
    BOOL isInside = FALSE;
    DWRITE_HIT_TEST_METRICS hitMetrics;
    textLayout->HitTestPoint(xOffset, 0.0f, &isTrailingHit, &isInside, &hitMetrics);

    return hitMetrics.textPosition + (isTrailingHit ? 1 : 0);
}

float TextMetrics::getLineHeight(const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    auto* factory = getDWriteFactory();

    if (!textFormat || !factory)
        return 0.0f;

    // Create a text layout with a single character to get metrics
    ComPtr<IDWriteTextLayout> textLayout;
    factory->CreateTextLayout(L"X", 1, textFormat, 10000.0f, 10000.0f,
                              textLayout.GetAddressOf());

    if (!textLayout)
        return 0.0f;

    DWRITE_LINE_METRICS lineMetrics;
    UINT32 lineCount = 0;
    textLayout->GetLineMetrics(&lineMetrics, 1, &lineCount);

    return lineMetrics.height;
}

float TextMetrics::getAscent(const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    auto* factory = getDWriteFactory();

    if (!textFormat || !factory)
        return 0.0f;

    // Create a text layout to get metrics
    ComPtr<IDWriteTextLayout> textLayout;
    factory->CreateTextLayout(L"X", 1, textFormat, 10000.0f, 10000.0f,
                              textLayout.GetAddressOf());

    if (!textLayout)
        return 0.0f;

    DWRITE_LINE_METRICS lineMetrics;
    UINT32 lineCount = 0;
    textLayout->GetLineMetrics(&lineMetrics, 1, &lineCount);

    return lineMetrics.baseline;
}

float TextMetrics::getDescent(const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    auto* factory = getDWriteFactory();

    if (!textFormat || !factory)
        return 0.0f;

    // Create a text layout to get metrics
    ComPtr<IDWriteTextLayout> textLayout;
    factory->CreateTextLayout(L"X", 1, textFormat, 10000.0f, 10000.0f,
                              textLayout.GetAddressOf());

    if (!textLayout)
        return 0.0f;

    DWRITE_LINE_METRICS lineMetrics;
    UINT32 lineCount = 0;
    textLayout->GetLineMetrics(&lineMetrics, 1, &lineCount);

    return lineMetrics.height - lineMetrics.baseline;
}

} // namespace eacp::Graphics
