#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// A layer's contract with the component that owns it and the host that renders
// it, which is the part nothing on screen would report.
//
// It finds its way into a frame the way a PathShape does: by registering with
// its component in its constructor, so the host can walk the tree and fill every
// one of them before the frame's own pass opens. Miss that and the layer is
// simply never rendered -- there is no texture to draw, so the group it holds is
// absent and nothing says why.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

auto tLayerRegisters = test("Layer/aLayerRegistersWithItsComponent") = []
{
    auto component = Component {};

    check(component.getLayers().size() == 0);

    {
        auto layer = Layer {component};

        check(component.getLayers().size() == 1);
        check(component.getLayers()[0] == &layer);
    }

    check(component.getLayers().size() == 0, "and is gone with it");
};

auto tLayerStartsEmpty = test("Layer/aLayerHasNothingInItUntilItIsRendered") = []
{
    auto component = Component {};
    auto layer = Layer {component};

    check(layer.isEmpty(), "so drawing it is a no-op rather than a black square");

    layer.setBounds({10.f, 20.f, 30.f, 40.f});

    check(sameRect(layer.getBounds(), {10.f, 20.f, 30.f, 40.f}));
    check(layer.isEmpty());
};

// The fade is applied where the layer is composited, so changing it is a repaint
// and not a re-render -- which is what makes an animated opacity affordable.
auto tOpacityDoesNotDirty = test("Layer/fadingALayerDoesNotRebuildIt") = []
{
    auto component = Component {};
    auto layer = Layer {component};

    layer.setOpacity(0.5f);

    check(layer.getOpacity() == 0.5f);

    layer.setOpacity(2.f);

    check(layer.getOpacity() == 1.f, "clamped, since it multiplies a colour");

    layer.setOpacity(-1.f);

    check(layer.getOpacity() == 0.f);
};

// Bounds it already has are not a change, because setting them asks for the
// content again and a layer re-rendered every frame for nothing is a render pass
// per frame for nothing.
auto tSameBoundsAreNotAChange = test("Layer/settingTheSameBoundsAsksForNothing") = []
{
    auto component = Component {};
    auto layer = Layer {component};

    layer.setBounds({0.f, 0.f, 50.f, 50.f});
    layer.setBounds({0.f, 0.f, 50.f, 50.f});

    check(sameRect(layer.getBounds(), {0.f, 0.f, 50.f, 50.f}));
};
