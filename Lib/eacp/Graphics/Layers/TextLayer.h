#pragma once

/** @file TextLayer: a retained single run of text in one font and color.
    EACP_HAS_CONTEXT only. */

#include "../Primitives/Font.h"
#include "Layer.h"

namespace eacp::Graphics
{

class TextLayer : public Layer
{
public:
    TextLayer();
    ~TextLayer() override;

    void setText(const std::string& text);
    void setFont(const Font& font);
    void setColor(const Color& color);

    void* getNativeLayer() override;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
