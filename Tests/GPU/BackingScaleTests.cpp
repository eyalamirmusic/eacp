#include "Common.h"

// Regression: View::backingScaleChanged did not reach a GPUView, so a
// CAMetalLayer kept a stale drawable size after a window changed display.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct ScaleView final : GPUView
{
    ScaleView()
    {
        onBackingScaleChanged = [this](float scale)
        {
            ++notifications;
            lastScale = scale;
        };
    }

    void render(Frame& frame) override { auto pass = frame.beginPass({}); }

    int notifications = 0;
    float lastScale = 0.f;
};

struct PlainView final : Graphics::View
{
    void backingScaleChanged() override { ++changes; }

    int changes = 0;
};
} // namespace

auto tScaleIsUsableImmediately = test("BackingScale/isPositiveBeforeLayout") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScaleView {};

    check(view.backingScale() > 0.f);
};

auto tScaleSurvivesLayout = test("BackingScale/staysPositiveAfterLayout") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScaleView {};
    view.setBounds({0.f, 0.f, 64.f, 64.f});

    check(view.backingScale() > 0.f);
};

auto tScaleMatchesRenderedPixels = test("BackingScale/matchesRenderedPixelSize") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScaleView {};
    view.setBounds({0.f, 0.f, 40.f, 30.f});

    auto scale = view.backingScale();
    auto image = view.renderToImage(scale);

    check(image.isValid());
    check(image.width() == (int) (40.f * scale));
    check(image.height() == (int) (30.f * scale));
};

auto tNotificationDefaultsToNoOp =
    test("BackingScale/notificationDefaultsToNoOp") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = GPUView {};

    check(static_cast<bool>(view.onBackingScaleChanged));
    view.onBackingScaleChanged(2.f); // must not crash
};

// Otherwise every live resize drag would rebuild the glyph atlas.
auto tNoNotificationWithoutChange = test("BackingScale/resizeDoesNotNotify") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScaleView {};

    view.setBounds({0.f, 0.f, 32.f, 32.f});
    view.notifications = 0;

    view.setBounds({0.f, 0.f, 64.f, 64.f});
    view.setBounds({0.f, 0.f, 128.f, 96.f});

    check(view.notifications == 0);
};

auto tHookIsVirtualOnView = test("BackingScale/hookIsVirtualOnBaseView") = []
{
    auto view = PlainView {};

    auto& asBase = static_cast<Graphics::View&>(view);
    asBase.backingScaleChanged();

    check(view.changes == 1);
};

auto tHookIsSafeOnGPUView = test("BackingScale/hookIsSafeOnGPUView") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ScaleView {};
    view.setBounds({0.f, 0.f, 48.f, 48.f});

    auto& asBase = static_cast<Graphics::View&>(view);
    asBase.backingScaleChanged();

    check(view.backingScale() > 0.f);
};
