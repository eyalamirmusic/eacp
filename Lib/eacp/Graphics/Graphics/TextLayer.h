#pragma once

#include "../Primitives/Font.h"
#include "Layer.h"

namespace eacp::Graphics
{

class TextLayer: public Layer
{
public:
    TextLayer();

    void setText(const std::string& text);
    void setFont(const Font& font);
    void setColor(const Color& color);

    void setBounds(const Rect& bounds);
    void setPosition(const Point& position);

    void* getNativeLayer() override;

    void setHidden(bool hidden);
    void setOpacity(float opacity);

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
