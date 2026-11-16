#include "Common.h"

#include <cmath>
#include <limits>

// GPUView::setRenderScale. The swapchain half - a smaller drawable on a surface
// that stretches one - needs a window and lives in PresentTests-Linux.cpp.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool near(float value, float expected)
{
    return std::abs(value - expected) < 1e-4f;
}
} // namespace

auto tDefaultsToOne = test("RenderScale/defaultsToOne") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = GPUView {};

    check(view.renderScale() == 1.f);
};

auto tIsClamped = test("RenderScale/isClampedToAQuarterAndOne") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = GPUView {};

    view.setRenderScale(0.5f);
    check(view.renderScale() == 0.5f);

    view.setRenderScale(0.1f);
    check(view.renderScale() == 0.25f);

    view.setRenderScale(3.f);
    check(view.renderScale() == 1.f);

    view.setRenderScale(-1.f);
    check(view.renderScale() == 0.25f);

    view.setRenderScale(std::numeric_limits<float>::quiet_NaN());
    check(view.renderScale() == 1.f);
};

// Metal and D3D12 size the drawable from the view itself, so the scale is in
// backingScale() before any window exists. The Vulkan backend can only know
// once a surface has said whether it stretches, and until then reports the
// surface's own scale.
auto tBackingScaleFollows = test("RenderScale/backingScaleFollowsWhereHonoured") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = GPUView {};
    view.setBounds({0.f, 0.f, 64.f, 64.f});

    const auto full = view.backingScale();

    view.setRenderScale(0.5f);

    if constexpr (Platform::isLinuxFamily())
    {
        check(view.backingScale() == full);
        return;
    }

    check(near(view.backingScale(), full * 0.5f));

    view.setRenderScale(1.f);

    check(near(view.backingScale(), full));
};

auto tSnapshotsKeepTheirScale =
    test("RenderScale/snapshotsRenderAtTheAskedScale") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = GPUView {};
    view.setBounds({0.f, 0.f, 40.f, 30.f});
    view.setRenderScale(0.5f);

    auto image = view.renderToImage(2.f);

    check(image.isValid());
    check(image.width() == 80);
    check(image.height() == 60);
};
