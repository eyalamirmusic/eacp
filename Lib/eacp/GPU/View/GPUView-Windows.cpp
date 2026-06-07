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

int GPUView::sampleCount() const
{
    return 1;
}

void GPUView::setSampleCount(int) {}

void GPUView::setContinuous(bool) {}

bool GPUView::isContinuous() const
{
    return false;
}

void GPUView::resized()
{
    Graphics::View::resized();
}

void GPUView::paint(Graphics::Context&) {}

void GPUView::renderNow() {}
} // namespace eacp::GPU
