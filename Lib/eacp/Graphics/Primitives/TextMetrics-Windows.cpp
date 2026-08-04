#include "TextMetrics.h"
#include "../D2D-Windows.h"
#include <eacp/Core/Utils/Strings.h>

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

    auto wideText = Strings::widen(text);

    auto textLayout = ComPtr<IDWriteTextLayout>();
    factory->CreateTextLayout(wideText.c_str(),
                              static_cast<UINT32>(wideText.length()),
                              textFormat,
                              10000.0f,
                              10000.0f,
                              textLayout.GetAddressOf());

    if (!textLayout)
        return 0.0f;

    auto metrics = DWRITE_TEXT_METRICS();
    textLayout->GetMetrics(&metrics);

    // `width` stops at the last ink, so a trailing space — or a lone one, as a
    // caller measuring codepoint by codepoint sends — measures zero and words
    // fuse. CTLine's typographic width counts trailing whitespace; match it.
    return metrics.widthIncludingTrailingWhitespace;
}

float TextMetrics::getOffsetForIndex(const std::string& text,
                                     size_t index,
                                     const Font& font)
{
    if (index == 0)
        return 0.0f;

    auto substring = text.substr(0, index);
    return measureWidth(substring, font);
}

size_t TextMetrics::getIndexForOffset(const std::string& text,
                                      float xOffset,
                                      const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    auto* factory = getDWriteFactory();

    if (!textFormat || !factory)
        return 0;

    auto wideText = Strings::widen(text);
    auto textLayout = ComPtr<IDWriteTextLayout>();
    factory->CreateTextLayout(wideText.c_str(),
                              static_cast<UINT32>(wideText.length()),
                              textFormat,
                              10000.0f,
                              10000.0f,
                              textLayout.GetAddressOf());

    if (!textLayout)
        return 0;

    BOOL isTrailingHit = FALSE;
    BOOL isInside = FALSE;
    auto hitMetrics = DWRITE_HIT_TEST_METRICS();
    textLayout->HitTestPoint(xOffset, 0.0f, &isTrailingHit, &isInside, &hitMetrics);

    return hitMetrics.textPosition + (isTrailingHit ? 1 : 0);
}

float TextMetrics::getLineHeight(const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    auto* factory = getDWriteFactory();

    if (!textFormat || !factory)
        return 0.0f;

    auto textLayout = ComPtr<IDWriteTextLayout>();
    factory->CreateTextLayout(
        L"X", 1, textFormat, 10000.0f, 10000.0f, textLayout.GetAddressOf());

    if (!textLayout)
        return 0.0f;

    auto lineMetrics = DWRITE_LINE_METRICS();
    auto lineCount = UINT32(0);
    textLayout->GetLineMetrics(&lineMetrics, 1, &lineCount);

    return lineMetrics.height;
}

float TextMetrics::getAscent(const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    auto* factory = getDWriteFactory();

    if (!textFormat || !factory)
        return 0.0f;

    auto textLayout = ComPtr<IDWriteTextLayout>();
    factory->CreateTextLayout(
        L"X", 1, textFormat, 10000.0f, 10000.0f, textLayout.GetAddressOf());

    if (!textLayout)
        return 0.0f;

    auto lineMetrics = DWRITE_LINE_METRICS();
    auto lineCount = UINT32(0);
    textLayout->GetLineMetrics(&lineMetrics, 1, &lineCount);

    return lineMetrics.baseline;
}

float TextMetrics::getDescent(const Font& font)
{
    auto* textFormat = static_cast<IDWriteTextFormat*>(font.getHandle());
    auto* factory = getDWriteFactory();

    if (!textFormat || !factory)
        return 0.0f;

    auto textLayout = ComPtr<IDWriteTextLayout>();
    factory->CreateTextLayout(
        L"X", 1, textFormat, 10000.0f, 10000.0f, textLayout.GetAddressOf());

    if (!textLayout)
        return 0.0f;

    auto lineMetrics = DWRITE_LINE_METRICS();
    auto lineCount = UINT32(0);
    textLayout->GetLineMetrics(&lineMetrics, 1, &lineCount);

    return lineMetrics.height - lineMetrics.baseline;
}

} // namespace eacp::Graphics
