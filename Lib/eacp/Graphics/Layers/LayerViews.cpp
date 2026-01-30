#include "LayerViews.h"

namespace eacp::Graphics
{

TextLayerView::TextLayerView(const std::string& text)
{
    addLayer(layer);
    layer.setFont(FontOptions().withName("Helvetica-Bold").withSize(14.f));
    layer.setColor({1.f, 1.f, 1.f});
    layer.setText(text);
}

void TextLayerView::resized()
{
    layer.setBounds(getLocalBounds());
}

ShapeLayerView::ShapeLayerView()
{
    addLayer(layer);
}

void ShapeLayerView::resized()
{
    layer.setBounds(getLocalBounds());
}
} // namespace eacp::Graphics