#include "Common.h"

#include <cmath>

// A render pipeline with no fragment stage (ShaderSource::withFragment with an
// empty entry): bgfx's createProgram(vsh, invalid), what a depth prepass, a
// shadow map or an occluder pass draws with. It writes depth and stencil and
// leaves every colour attachment alone. Depth rides on a colour target, so the
// pass still has one; the pipeline keeps its format and writes nothing to it.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace VertexOnlyPipelineTests
{
namespace
{
constexpr auto targetWidth = 16;
constexpr auto targetHeight = 4;
constexpr auto writtenDepth = 0.25f;

constexpr std::uint32_t opaqueBlue = 0xffff0000;
constexpr std::uint32_t opaqueRed = 0xff0000ff;
constexpr std::uint32_t opaqueBlack = 0xff000000;

struct QuadVertex
{
    float position[2];
    float uv[2];
};

constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

constexpr QuadVertex leftHalf[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{0.f, -1.f}, {0.5f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{0.f, -1.f}, {0.5f, 1.f}},
    {{0.f, 1.f}, {0.5f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

ShaderLibrary makeDepthOnlyLibrary()
{
    return Device::shared().makeShaderLibrary(
        nativeVertexOnlySource("VertexOnlyPipeline/depthOnly"));
}

ShaderSource solidSource()
{
    return nativeShaderSource("VertexOnlyPipeline/solid")
        .withVertex("vertexMain")
        .withFragment("fragmentMain");
}

RenderPipelineDescriptor describePositionOnly(const ShaderLibrary& library)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
    descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
    descriptor.vertexLayout.stride = sizeof(QuadVertex);
    return descriptor;
}

RenderPipeline makeDepthWriter(const ShaderLibrary& library)
{
    auto descriptor = describePositionOnly(library);
    descriptor.depth = true;
    descriptor.depthCompare = DepthCompare::Less;
    return Device::shared().makeRenderPipeline(descriptor);
}

RenderPipeline makeStencilMarker(const ShaderLibrary& library)
{
    auto descriptor = describePositionOnly(library);
    descriptor.stencil = true;
    descriptor.stencilFront.pass = StencilOp::Replace;
    descriptor.stencilBack.pass = StencilOp::Replace;
    return Device::shared().makeRenderPipeline(descriptor);
}

RenderPipeline makeStencilTested(const ShaderLibrary& library)
{
    auto descriptor = describePositionOnly(library);
    descriptor.stencil = true;
    descriptor.stencilFront.compare = CompareFunction::Equal;
    descriptor.stencilBack.compare = CompareFunction::Equal;
    return Device::shared().makeRenderPipeline(descriptor);
}

TextureDescriptor describeScene(bool stencil)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetWidth;
    descriptor.height = targetHeight;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    descriptor.sampleableDepth = !stencil;
    descriptor.stencil = stencil;
    return descriptor;
}

Vector<std::uint32_t> readPixels(const Texture& texture)
{
    auto pixels = Vector<std::uint32_t> {};
    pixels.resize(targetWidth * targetHeight, 0u);
    texture.read(pixels.data());
    return pixels;
}

int countOff(const Vector<std::uint32_t>& pixels,
             std::uint32_t leftExpected,
             std::uint32_t rightExpected)
{
    auto mismatches = 0;

    for (auto y = 0; y < targetHeight; ++y)
        for (auto x = 0; x < targetWidth; ++x)
        {
            const auto expected = x < targetWidth / 2 ? leftExpected : rightExpected;

            if (pixels[y * targetWidth + x] != expected)
                ++mismatches;
        }

    return mismatches;
}

// Pass one draws the left half through the vertex-only pipeline into a target
// cleared to blue; pass two shows the depth it left as red.
struct DepthOnlyView final : GPUView
{
    DepthOnlyView()
        : scene(Device::shared().makeTexture(describeScene(false)))
        , depthOnly(makeDepthOnlyLibrary())
        , writer(makeDepthWriter(depthOnly))
        , showDepth(Device::shared().makeShaderLibrary(
              nativeShaderSource("VertexOnlyPipeline/showDepth")
                  .withVertex("vertexMain")
                  .withFragment("fragmentMain")))
        , reader(makeReader())
        , left(Device::shared().makeBuffer(leftHalf))
        , full(Device::shared().makeBuffer(fullQuad))
        , sampler(Device::shared().makeSampler({}))
    {
    }

    RenderPipeline makeReader()
    {
        setSampleCount(1);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &showDepth;
        descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
        descriptor.vertexLayout.attribute(VertexFormat::Float2, 8);
        descriptor.vertexLayout.stride = sizeof(QuadVertex);
        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = Graphics::Color {0.f, 0.f, 1.f, 1.f};
            descriptor.depthAction = DepthAction::Keep;

            auto into = frame.beginPass(scene, descriptor);
            into.setPipeline(writer);
            into.setVertexBuffer(left);
            into.draw(6);
        }

        auto pass = frame.beginPass({Graphics::Color::black()});
        pass.setPipeline(reader);
        pass.setVertexBuffer(full);
        pass.setFragmentDepthTexture(scene, 0, sampler);
        pass.draw(6);
    }

    Graphics::Image readBack()
    {
        setBounds({0.f, 0.f, (float) targetWidth, (float) targetHeight});
        return renderToImage(1.f);
    }

    bool isReady() const
    {
        return scene.hasSampleableDepth() && writer.isValid() && reader.isValid()
               && sampler.isValid();
    }

    Texture scene;
    ShaderLibrary depthOnly;
    RenderPipeline writer;
    ShaderLibrary showDepth;
    RenderPipeline reader;
    Buffer left;
    Buffer full;
    Sampler sampler;
};

// Pass one marks the left half's stencil through the vertex-only pipeline;
// pass two resumes the plane and draws red only where it was marked.
struct StencilOnlyView final : GPUView
{
    StencilOnlyView()
        : scene(Device::shared().makeTexture(describeScene(true)))
        , vertexOnly(
              Device::shared().makeShaderLibrary(solidSource().withFragment({})))
        , solid(Device::shared().makeShaderLibrary(solidSource()))
        , marker(makeStencilMarker(vertexOnly))
        , tested(makeStencilTested(solid))
        , left(Device::shared().makeBuffer(leftHalf))
        , full(Device::shared().makeBuffer(fullQuad))
    {
    }

    void render(Frame& frame) override
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = Graphics::Color {0.f, 0.f, 1.f, 1.f};
            descriptor.depthAction = DepthAction::Keep;

            auto into = frame.beginPass(scene, descriptor);
            into.setPipeline(marker);
            into.setStencilReference(1);
            into.setVertexBuffer(left);
            into.draw(6);
        }

        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = Graphics::Color::black();
            descriptor.depthAction = DepthAction::Resume;

            auto into = frame.beginPass(scene, descriptor);
            into.setPipeline(tested);
            into.setStencilReference(1);
            into.setVertexBuffer(full);
            into.draw(6);
        }

        frame.beginPass({Graphics::Color::black()});
    }

    void run()
    {
        setBounds({0.f, 0.f, (float) targetWidth, (float) targetHeight});
        renderToImage(1.f);
    }

    bool isReady() const
    {
        return scene.hasStencil() && marker.isValid() && tested.isValid();
    }

    Texture scene;
    ShaderLibrary vertexOnly;
    ShaderLibrary solid;
    RenderPipeline marker;
    RenderPipeline tested;
    Buffer left;
    Buffer full;
};
} // namespace

auto tLibraryWithoutFragment =
    test("VertexOnlyPipeline/aLibraryWithNoFragmentEntryBuildsAPipeline") = []
{
    if (!Device::shared().isValid())
        return;

    auto library = makeDepthOnlyLibrary();

    check(library.isValid());
    check(!library.hasFragment());
    check(makeDepthWriter(library).isValid());
};

auto tFragmentStillRequired =
    test("VertexOnlyPipeline/aNamedFragmentEntryIsStillRequired") = []
{
    if (!Device::shared().isValid())
        return;

    auto source = nativeVertexOnlySource("VertexOnlyPipeline/depthOnly");
    auto library =
        Device::shared().makeShaderLibrary(source.withFragment("fragmentMain"));

    check(library.hasFragment());

    if (library.isValid())
        check(!makeDepthWriter(library).isValid());
};

auto tDepthOnly = test("VertexOnlyPipeline/writesDepthAndLeavesTheColour") = []
{
    if (!Device::shared().isValid() || !Device::shared().supportsPerBindSamplers())
        return;

    auto view = DepthOnlyView {};

    if (!view.isReady())
        return;

    auto image = view.readBack();

    check(image.isValid());

    auto mismatches = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
        {
            const auto expected = x < image.width() / 2 ? writtenDepth : 1.f;

            if (std::abs(image.at(x, y).r - expected) > 0.02f)
                ++mismatches;
        }

    check(mismatches == 0);
    check(countOff(readPixels(view.scene), opaqueBlue, opaqueBlue) == 0);
};

auto tStencilOnly = test("VertexOnlyPipeline/writesStencilAndLeavesTheColour") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = StencilOnlyView {};

    if (!view.isReady())
        return;

    view.run();

    check(countOff(readPixels(view.scene), opaqueRed, opaqueBlack) == 0);
};
} // namespace VertexOnlyPipelineTests
