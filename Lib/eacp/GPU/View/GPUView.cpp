#include "GPUView.h"

#include <algorithm>
#include <cmath>

namespace eacp::GPU
{
void GPUView::render(Frame&) {}

void GPUView::update(Threads::FrameTime) {}

float GPUView::clampRenderScale(float scale)
{
    if (std::isnan(scale))
        return 1.f;

    return std::clamp(scale, 0.25f, 1.f);
}
} // namespace eacp::GPU
