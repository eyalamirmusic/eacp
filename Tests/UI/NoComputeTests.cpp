#include <eacp/Core/Utils/Environment.h>
#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <optional>
#include <string>

// An interface on a device with no compute tier. The coverage atlas is written
// by a kernel, so where there is no kernel every vector shape takes the
// triangle route PathShape::buildMesh already implements and the atlas holds
// nothing but the opaque texel every unmasked shape samples - which leaves the
// fragment stage's multiply exactly as it was. See D10 of the OpenGL plan;
// EACP_GPU_NO_COMPUTE=1 is what makes the route reachable on a device that has
// kernels.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
const auto blue = Color {0.f, 0.f, 1.f, 1.f};

bool isNear(float a, float b, float tolerance = 0.02f)
{
    return std::abs(a - b) <= tolerance;
}

bool isColour(const Color& actual, const Color& expected)
{
    return isNear(actual.r, expected.r) && isNear(actual.g, expected.g)
           && isNear(actual.b, expected.b) && actual.a > 0.9f;
}

// Puts back what was there, so a host built after this one is built against the
// device the rest of the suite runs on.
struct ScopedEnv
{
    ScopedEnv(std::string_view nameToUse, std::string_view value)
        : name(nameToUse)
        , previous(getEnv(nameToUse))
    {
        setEnv(name, value);
    }

    ~ScopedEnv()
    {
        if (previous.has_value())
            setEnv(name, *previous);
        else
            unsetEnv(name);
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    std::string name;
    std::optional<std::string> previous;
};

// Backing::Mask on purpose: it is the one answer the no-compute route has to
// override, and at this size Automatic would ask for a mask too.
struct MaskedShape final : Component
{
    void resized() override
    {
        auto path = GPUWidgets::Path {};
        path.addRect({50.f, 25.f, 100.f, 100.f});

        shape.setBacking(PathShape::Backing::Mask);
        shape.setPath(path);
    }

    void paint(UI::Graphics& g) override
    {
        g.setColour(blue);
        g.fillPath(shape);
    }

    PathShape shape {*this};
};

struct Rendered
{
    float atlasFill = 0.f;
    int meshed = 0;
    bool drawn = false;
    bool isMeshed = false;
};

Rendered renderOnce()
{
    auto host = ComponentHost {};
    auto content = MaskedShape {};

    host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
    host.setBounds({0.f, 0.f, 200.f, 150.f});
    host.resized();
    host.setRootComponent(content);

    const auto scale = 1.f;
    auto image = host.renderToImage(scale);

    return {host.getAtlasFillFraction(),
            host.getLastMeshedPathCount(),
            isColour(image.at(100, 75), blue),
            content.shape.isMeshed()};
}
} // namespace

// The route itself: the same tree, once on the device as it is and once with
// the compute tier taken away.
auto tMeshesWithoutCompute =
    test("ComponentHost/aHostWithoutComputeMeshesEveryPath") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    // The first half is what a device with a kernel tier does. A backend
    // without one - OpenGL, until its compute tier is built - is the second
    // half all the way through, and takes the mesh route below without being
    // told to, so there is nothing of the first half to compare against.
    const auto hasKernels = GPU::Device::shared().supportsCompute();
    const auto withCompute = hasKernels ? renderOnce() : Rendered {};

    if (hasKernels)
    {
        check(!withCompute.isMeshed,
              "a masked shape is a mask where a kernel runs");
        check(withCompute.meshed == 0);
        check(withCompute.drawn, "and it draws");
    }

    auto withoutCompute = ScopedEnv {"EACP_GPU_NO_COMPUTE", "1"};

    check(!GPU::Device::shared().supportsCompute());

    const auto meshedOnly = renderOnce();

    check(meshedOnly.isMeshed, "the mask backing is overridden by the device");
    check(meshedOnly.meshed == 1, "and the host counts it as meshed");
    check(meshedOnly.drawn, "the shape is still drawn, as triangles");

    // Nothing was rasterized into it, so what it holds is the opaque corner it
    // is seeded with and nothing else.
    if (hasKernels)
        check(meshedOnly.atlasFill < withCompute.atlasFill,
              "the atlas is left empty but for its opaque texel");
};
