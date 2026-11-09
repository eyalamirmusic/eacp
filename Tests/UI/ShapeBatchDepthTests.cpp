#include "Common.h"

#include <NanoTest/NanoTest.h>

#include <eacp/UI/Render/GradientRamps.h>
#include <eacp/UI/Render/ShapeBatch.h>

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace ShapeBatchDepthTests
{
constexpr auto viewSize = 64.f;
constexpr auto pixelScale = 1.f;
constexpr auto singleSampleCount = 1;
constexpr auto filledRectInset = 8.f;
constexpr auto minimumBrightRed = 0.9f;

struct DepthPassShapeView final : GPU::GPUView
{
    DepthPassShapeView()
    {
        setSampleCount(singleSampleCount);
        setDepth(true);
        setBounds({0.f, 0.f, viewSize, viewSize});
        batch.setPassHasDepthAttachment(true);
    }

    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        const auto filledSize = viewSize - 2.f * filledRectInset;

        batch.begin(pass);
        batch.fillRect({filledRectInset, filledRectInset, filledSize, filledSize},
                       Color::red());
        batch.end();
    }

    CoverageAtlas atlas;
    GradientRamps ramps;
    ShapeBatch batch {
        atlas, ramps, {viewSize, viewSize}, pixelScale, singleSampleCount};
};

auto tDrawsIntoAPassWithADepthAttachment =
    test("ShapeBatch/drawsIntoAPassWithADepthAttachment") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto view = DepthPassShapeView {};
    auto image = view.renderToImage(pixelScale);
    const auto centre = static_cast<int>(viewSize / 2.f);

    check(image.isValid());
    check(image.at(centre, centre).r > minimumBrightRed);
};
} // namespace ShapeBatchDepthTests
