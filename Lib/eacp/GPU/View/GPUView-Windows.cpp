#include "GPUView.h"

// Windows/DirectX stub. The GPUView slots into the View hierarchy but renders
// nothing yet; a real D3D12 swapchain implementation goes here later.

namespace eacp::GPU
{
struct GPUView::Native
{
};

GPUView::GPUView()
    : impl()
{
}

GPUView::~GPUView() = default;

void GPUView::resized()
{
    Graphics::View::resized();
}

void GPUView::renderTick() {}
} // namespace eacp::GPU
