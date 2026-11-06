#pragma once

/** @file Views that each own one TextLayer or ShapeLayer sized to their bounds.
    EACP_HAS_CONTEXT only. */

#include "TextLayer.h"
#include "ShapeLayer.h"
#include "../View/View.h"

namespace eacp::Graphics
{

struct TextLayerView : View
{
    TextLayerView(const std::string& text = {});

    void resized() override;

    constexpr TextLayer* operator->() { return &layer; }

    TextLayer layer;
};

struct ShapeLayerView : View
{
    ShapeLayerView();

    void resized() override;

    constexpr ShapeLayer* operator->() { return &layer; }

    ShapeLayer layer;
};
} // namespace eacp::Graphics
