#pragma once

#include "../Primitives/Font.h"

namespace eacp::Graphics
{

class View;

class TextLayer
{
public:
    TextLayer();

    void setText(const std::string& text);
    void setFont(const Font& font);
    void setColor(const Color& color);

    void setBounds(const Rect& bounds);
    void setPosition(const Point& position);

    void setHidden(bool hidden);
    void setOpacity(float opacity);

private:
    friend class View;
    void attachToLayer(void* nativeLayer);
    void detachFromLayer();

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
