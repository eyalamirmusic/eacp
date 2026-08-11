#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

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

auto tSameBoundsAreNotAChange = test("Layer/settingTheSameBoundsAsksForNothing") = []
{
    auto component = Component {};
    auto layer = Layer {component};

    layer.setBounds({0.f, 0.f, 50.f, 50.f});
    layer.setBounds({0.f, 0.f, 50.f, 50.f});

    check(sameRect(layer.getBounds(), {0.f, 0.f, 50.f, 50.f}));
};
