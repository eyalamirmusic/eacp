#include "Common.h"

// GPUView::backingScale and the onBackingScaleChanged notification.
//
// The scale itself is whatever display the test machine has, so these cannot
// assert a number. What they can pin down is the contract around it: that the
// value is always usable, that the notification hook is safe to call
// unconditionally, and that View::backingScaleChanged is virtual and reaches a
// GPUView -- the route that was missing, and the reason a CAMetalLayer kept a
// stale drawable size after a window moved between displays.

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

// Records backingScaleChanged on a plain View, to prove the hook is virtual and
// dispatches without any GPU involvement.
struct PlainView final : Graphics::View
{
    void backingScaleChanged() override { ++changes; }

    int changes = 0;
};
} // namespace

// Readable before the view is ever laid out: a glyph atlas is sized in the
// constructor, long before the first resize, and a zero there would be a
// division by zero rather than a wrong-looking atlas.
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

// The scale relates logical points to the pixels a snapshot is made of, so
// rendering at the view's own scale must produce exactly bounds * scale pixels.
// This is the relationship a caller relies on to turn a logical rect into the
// pixel rect setScissorRect wants.
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

// The callback is never null, so the framework can fire it without a null check
// and an app that ignores scale changes needs no boilerplate.
auto tNotificationDefaultsToNoOp = test("BackingScale/notificationDefaultsToNoOp") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = GPUView {};

    check(static_cast<bool>(view.onBackingScaleChanged));
    view.onBackingScaleChanged(2.f); // must not crash
};

// A resize at an unchanged scale is not a scale change. Otherwise every live
// drag would rebuild the glyph atlas.
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

// backingScaleChanged is virtual on the base View, which is what lets the
// platform layer notify any view without knowing whether it is GPU-backed.
auto tHookIsVirtualOnView = test("BackingScale/hookIsVirtualOnBaseView") = []
{
    auto view = PlainView {};

    auto& asBase = static_cast<Graphics::View&>(view);
    asBase.backingScaleChanged();

    check(view.changes == 1);
};

// The same call on a GPUView re-syncs the drawable and stays safe to invoke
// directly -- the platform hook calls exactly this.
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
