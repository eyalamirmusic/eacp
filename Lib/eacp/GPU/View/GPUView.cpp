#include "GPUView.h"

#include <algorithm>
#include <cmath>

namespace eacp::GPU
{
void GPUView::render(Frame&) {}

void GPUView::update(Threads::FrameTime) {}

void GPUView::deviceRestored() {}

namespace
{
constexpr auto lowestRenderScale = 0.25f;
constexpr auto fullRenderScale = 1.f;
} // namespace

float GPUView::clampRenderScale(float scale)
{
    if (std::isnan(scale))
        return fullRenderScale;

    return std::clamp(scale, lowestRenderScale, fullRenderScale);
}
} // namespace eacp::GPU
