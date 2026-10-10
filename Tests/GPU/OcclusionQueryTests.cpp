#include "Common.h"

#include <cstdint>
#include <functional>

// RenderPass::beginOcclusionQuery and OcclusionQueries: what a draw counts when
// it is hidden behind a nearer one, when it is not, and when the target is
// multisampled; the any-samples form; several queries in one pass, in two
// passes of one frame and over two frames; and the calls a pass refuses.
//
// The geometry is two quads at fixed depths over a 32-pixel square target: the
// whole target, or its left half (16 x 32 = 512 pixels). A count is exact where
// Device::supportsOcclusionSampleCounts() says so, and only non-zero elsewhere.
//
// Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewSize = 32;
constexpr auto leftHalfPixels = std::uint64_t {viewSize / 2 * viewSize};

constexpr auto nearDepth = 0.25f;
constexpr auto farDepth = 0.75f;

struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

constexpr QuadVertex leftQuad[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},
};

struct DepthQuad final : ShaderProgram
{
    DepthQuad() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, depth, 1.f));
        setFragment(float4(depth, depth, depth, 1.f));
    }

    Uniform<Float> depth;

    EACP_SHADER(depth)
};

bool exactCounts()
{
    return Device::shared().supportsOcclusionSampleCounts();
}

// A count that is the expected one where the device counts exactly, and merely
// non-zero where it does not.
bool counted(std::uint64_t value, std::uint64_t expected)
{
    return exactCounts() ? value == expected : value != 0;
}

// The pass's descriptor names `queries`; `body` records the draws. The
// occluder covers the whole target near, the probe the left half far, and the
// front probe the left half near.
struct QueryView final : GPUView
{
    QueryView(OcclusionQueries& queriesToUse, int samples = 1)
        : queries(&queriesToUse)
    {
        setSampleCount(samples);
        setDepth(true);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.sampleCount = sampleCount();
        descriptor.depth = true;
        descriptor.depthCompare = CompareFunction::Less;
        descriptor.depthWrite = true;

        occluder.depth = nearDepth;
        occluder.setVertices(fullQuad);
        occluder.prepare(descriptor);

        probe.depth = farDepth;
        probe.setVertices(leftQuad);
        probe.prepare(descriptor);

        frontProbe.depth = 0.1f;
        frontProbe.setVertices(leftQuad);
        frontProbe.prepare(descriptor);
    }

    void render(Frame& frame) override
    {
        auto descriptor = RenderPassDescriptor {};
        descriptor.clearColor = Graphics::Color::black();
        descriptor.occlusionQueries = queries;

        auto pass = frame.beginPass(descriptor);
        body(pass);
    }

    void run()
    {
        setBounds({0.f, 0.f, (float) viewSize, (float) viewSize});
        renderToImage(1.f);
    }

    OcclusionQueries* queries = nullptr;
    DepthQuad occluder;
    DepthQuad probe;
    DepthQuad frontProbe;
    std::function<void(RenderPass&)> body = [](RenderPass&) {};
};
} // namespace

auto tOcclusionDeviceAnswers = test("Occlusion/theDeviceAnswers") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    check(device.supportsOcclusionQueries());

    auto none = OcclusionQueries {device, 0};
    check(!none.isValid());
    check(none.size() == 0);

    auto queries = OcclusionQueries {device, 8};
    check(queries.isValid());
    check(queries.size() == 8);

    // Nothing has written them: complete, and zero throughout.
    check(queries.isComplete());

    std::uint64_t values[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    queries.read(values);

    for (auto value: values)
        check(value == 0);

    check(queries.result(8) == 0);
    check(queries.result(-1) == 0);
};

auto tOcclusionVisibleCounts = test("Occlusion/aVisibleDrawCountsItsSamples") = []
{
    if (!Device::shared().isValid())
        return;

    auto queries = OcclusionQueries {Device::shared(), 4};
    auto view = QueryView {queries};

    view.body = [&view](RenderPass& pass)
    {
        pass.beginOcclusionQuery(0);
        pass.draw(view.probe);
        pass.endOcclusionQuery();
    };

    view.run();

    check(queries.isComplete());
    check(counted(queries.result(0), leftHalfPixels));
};

// The occluder first, then the probe behind it inside a query: nothing passes.
// And in the same pass a second query around the front probe, which does.
auto tOcclusionHiddenCountsZero = test("Occlusion/anOccludedDrawCountsZero") = []
{
    if (!Device::shared().isValid())
        return;

    auto queries = OcclusionQueries {Device::shared(), 4};
    auto view = QueryView {queries};

    view.body = [&view](RenderPass& pass)
    {
        pass.beginOcclusionQuery(2);
        pass.draw(view.occluder);
        pass.endOcclusionQuery();

        pass.beginOcclusionQuery(1);
        pass.draw(view.probe);
        pass.endOcclusionQuery();

        pass.beginOcclusionQuery(3);
        pass.draw(view.frontProbe);
        pass.endOcclusionQuery();
    };

    view.run();

    std::uint64_t values[4] = {};
    queries.read(values);

    check(values[0] == 0);
    check(values[1] == 0);
    check(counted(values[2], leftHalfPixels * 2));
    check(counted(values[3], leftHalfPixels));
};

auto tOcclusionAnySamples = test("Occlusion/anySamplesSaysWhetherAnyPassed") = []
{
    if (!Device::shared().isValid())
        return;

    auto queries = OcclusionQueries {Device::shared(), 2};
    auto view = QueryView {queries};

    view.body = [&view](RenderPass& pass)
    {
        pass.draw(view.occluder);

        pass.beginOcclusionQuery(0, OcclusionMode::AnySamples);
        pass.draw(view.probe);
        pass.endOcclusionQuery();

        pass.beginOcclusionQuery(1, OcclusionMode::AnySamples);
        pass.draw(view.frontProbe);
        pass.endOcclusionQuery();
    };

    view.run();

    check(queries.result(0) == 0);
    check(queries.result(1) != 0);
};

// Samples, not pixels: four of them a pixel at 4x.
auto tOcclusionMultisampled = test("Occlusion/aMultisampledTargetCountsSamples") = []
{
    if (!Device::shared().isValid())
        return;

    auto queries = OcclusionQueries {Device::shared(), 1};
    auto view = QueryView {queries, 4};

    view.body = [&view](RenderPass& pass)
    {
        pass.beginOcclusionQuery(0);
        pass.draw(view.probe);
        pass.endOcclusionQuery();
    };

    view.run();

    check(counted(queries.result(0), leftHalfPixels * 4));
};

// Frame one writes query 0, frame two queries 1 and 2: what frame one wrote is
// still there after frame two, which never named it.
auto tOcclusionAcrossFrames = test("Occlusion/resultsSurviveTheNextFrame") = []
{
    if (!Device::shared().isValid())
        return;

    auto queries = OcclusionQueries {Device::shared(), 3};
    auto view = QueryView {queries};

    view.body = [&view](RenderPass& pass)
    {
        pass.beginOcclusionQuery(0);
        pass.draw(view.probe);
        pass.endOcclusionQuery();
    };

    view.run();
    check(counted(queries.result(0), leftHalfPixels));

    view.body = [&view](RenderPass& pass)
    {
        pass.draw(view.occluder);

        pass.beginOcclusionQuery(1);
        pass.draw(view.probe);
        pass.endOcclusionQuery();

        pass.beginOcclusionQuery(2);
        pass.draw(view.frontProbe);
        pass.endOcclusionQuery();
    };

    view.run();

    check(queries.isComplete());
    check(counted(queries.result(0), leftHalfPixels));
    check(queries.result(1) == 0);
    check(counted(queries.result(2), leftHalfPixels));

    // And a third frame that reuses an index overwrites it.
    view.body = [&view](RenderPass& pass)
    {
        pass.beginOcclusionQuery(0);
        pass.draw(view.occluder);
        pass.endOcclusionQuery();
    };

    view.run();

    check(counted(queries.result(0), leftHalfPixels * 2));
    check(counted(queries.result(2), leftHalfPixels));
};

// Two passes of one frame, the second resuming the first's depth: each writes
// its own index and the second leaves the first's alone.
auto tOcclusionTwoPasses = test("Occlusion/twoPassesOfOneFrameShareASet") = []
{
    if (!Device::shared().isValid())
        return;

    struct TwoPassView final : GPUView
    {
        explicit TwoPassView(OcclusionQueries& queriesToUse)
            : queries(&queriesToUse)
        {
            setSampleCount(1);
            setDepth(true);

            auto descriptor = RenderPipelineDescriptor {};
            descriptor.sampleCount = 1;
            descriptor.depth = true;
            descriptor.depthCompare = CompareFunction::Less;

            occluder.depth = nearDepth;
            occluder.setVertices(fullQuad);
            occluder.prepare(descriptor);

            probe.depth = farDepth;
            probe.setVertices(leftQuad);
            probe.prepare(descriptor);
        }

        void render(Frame& frame) override
        {
            {
                auto descriptor = RenderPassDescriptor {};
                descriptor.depthAction = DepthAction::Keep;
                descriptor.occlusionQueries = queries;

                auto pass = frame.beginPass(descriptor);
                pass.beginOcclusionQuery(0);
                pass.draw(probe);
                pass.endOcclusionQuery();
                pass.draw(occluder);
            }

            auto descriptor = RenderPassDescriptor {};
            descriptor.clear = false;
            descriptor.depthAction = DepthAction::Resume;
            descriptor.occlusionQueries = queries;

            auto pass = frame.beginPass(descriptor);
            pass.beginOcclusionQuery(1);
            pass.draw(probe);
            pass.endOcclusionQuery();
        }

        OcclusionQueries* queries = nullptr;
        DepthQuad occluder;
        DepthQuad probe;
    };

    auto queries = OcclusionQueries {Device::shared(), 2};
    auto view = TwoPassView {queries};
    view.setBounds({0.f, 0.f, (float) viewSize, (float) viewSize});
    view.renderToImage(1.f);

    check(counted(queries.result(0), leftHalfPixels));
    check(queries.result(1) == 0);
};

// A pass into a texture target counts like the drawable's.
auto tOcclusionTextureTarget = test("Occlusion/aTextureTargetPassCounts") = []
{
    if (!Device::shared().isValid())
        return;

    auto textureDescriptor = TextureDescriptor {};
    textureDescriptor.width = viewSize;
    textureDescriptor.height = viewSize;
    textureDescriptor.format = TextureFormat::RGBA8Unorm;
    textureDescriptor.renderTarget = true;
    textureDescriptor.depth = true;

    auto target = Device::shared().makeTexture(textureDescriptor);

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    descriptor.depth = true;
    descriptor.depthCompare = CompareFunction::Less;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);

    auto occluder = DepthQuad {};
    occluder.depth = nearDepth;
    occluder.setVertices(fullQuad);
    occluder.prepare(descriptor);

    auto probe = DepthQuad {};
    probe.depth = farDepth;
    probe.setVertices(leftQuad);
    probe.prepare(descriptor);

    auto queries = OcclusionQueries {Device::shared(), 2};

    struct Runner final : GPUView
    {
        void render(Frame& frame) override { draw(frame); }
        std::function<void(Frame&)> draw = [](Frame&) {};
    };

    auto runner = Runner {};
    runner.setSampleCount(1);
    runner.draw = [&](Frame& frame)
    {
        auto passDescriptor = RenderPassDescriptor {};
        passDescriptor.occlusionQueries = &queries;

        auto pass = frame.beginPass(target, passDescriptor);
        pass.beginOcclusionQuery(0);
        pass.draw(probe);
        pass.endOcclusionQuery();

        pass.draw(occluder);

        pass.beginOcclusionQuery(1);
        pass.draw(probe);
        pass.endOcclusionQuery();
    };
    runner.setBounds({0.f, 0.f, 4.f, 4.f});
    runner.renderToImage(1.f);

    check(counted(queries.result(0), leftHalfPixels));
    check(queries.result(1) == 0);
};

// What a pass refuses counts nothing: a second query while one is open, an
// index already counted in the pass, one outside the set, and any query on a
// pass that named no set. A query left open is closed by the pass's end.
auto tOcclusionRefusals = test("Occlusion/refusedQueriesCountNothing") = []
{
    if (!Device::shared().isValid())
        return;

    auto queries = OcclusionQueries {Device::shared(), 3};
    auto view = QueryView {queries};

    view.body = [&view](RenderPass& pass)
    {
        pass.beginOcclusionQuery(0);
        pass.beginOcclusionQuery(1); // refused: 0 is open
        pass.draw(view.probe);
        pass.endOcclusionQuery();

        pass.beginOcclusionQuery(0); // refused: already counted
        pass.beginOcclusionQuery(3); // refused: outside the set
        pass.beginOcclusionQuery(-1);
        pass.draw(view.frontProbe);
        pass.endOcclusionQuery(); // none open: nothing to do

        pass.beginOcclusionQuery(2); // left open, closed by end()
        pass.draw(view.occluder);
    };

    view.run();

    check(counted(queries.result(0), leftHalfPixels));
    check(queries.result(1) == 0);
    check(counted(queries.result(2), leftHalfPixels));

    // A pass that named no set refuses every query, and draws as usual.
    view.queries = nullptr;
    view.body = [&view](RenderPass& pass)
    {
        pass.beginOcclusionQuery(1);
        pass.draw(view.probe);
        pass.endOcclusionQuery();
    };

    view.run();

    check(queries.result(1) == 0);
};
