#pragma once

#include "TextLayer.h"
#include "ShapeLayer.h"
#include "../View/View.h"

namespace eacp::Graphics
{

struct TextLayerView : View
{
    TextLayerView(const std::string& text = {});

    void resized() override;

    TextLayer* operator->() { return &layer; }

    TextLayer layer;
};

struct ShapeLayerView : View
{
    ShapeLayerView();

    void resized() override;

    ShapeLayer* operator->() { return &layer; }

    ShapeLayer layer;
};
} // namespace eacp::Graphics
