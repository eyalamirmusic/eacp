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

const char* mslDepthOnly = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
};

struct VertexOut
{
    float4 position [[position]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.25, 1.0);
    return out;
}
)";

const char* hlslDepthOnly = R"(
float4 vertexMain(float2 position : TEXCOORD0) : SV_Position
{
    return float4(position, 0.25, 1.0);
}
)";

const char* glslDepthOnly = R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;

void main()
{
    gl_Position = vec4(attr0, 0.25, 1.0);
}
#endif
)";

const char* mslSolid = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
};

struct VertexOut
{
    float4 position [[position]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.25, 1.0);
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]])
{
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

const char* hlslSolid = R"(
float4 vertexMain(float2 position : TEXCOORD0) : SV_Position
{
    return float4(position, 0.25, 1.0);
}

float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

const char* glslSolid = R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;

void main()
{
    gl_Position = vec4(attr0, 0.25, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
#endif
)";

const char* mslShowDepth = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.uv = in.uv;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             depth2d<float> depthImage [[texture(0)]],
                             sampler depthSampler [[sampler(0)]])
{
    return float4(depthImage.sample(depthSampler, in.uv), 0.0, 0.0, 1.0);
}
)";

const char* hlslShowDepth = R"(
Texture2D<float> depthImage : register(t0);
SamplerState depthSampler : register(s8);

struct VertexIn
{
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 0.0, 1.0);
    o.uv = input.uv;
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return float4(depthImage.Sample(depthSampler, input.uv), 0.0, 0.0, 1.0);
}
)";

const char* glslShowDepth = R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec2 attr1;
layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
    uv = attr1;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 uv;
layout(set = 0, binding = 8) uniform sampler2D depthImage;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(texture(depthImage, uv).r, 0.0, 0.0, 1.0);
}
#endif
)";

ShaderLibrary makeDepthOnlyLibrary()
{
    return Device::shared().makeShaderLibrary(
        nativeVertexOnlySource(mslDepthOnly, hlslDepthOnly, glslDepthOnly));
}

ShaderSource solidSource()
{
    return nativeShaderSource(mslSolid, hlslSolid, glslSolid)
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
              nativeShaderSource(mslShowDepth, hlslShowDepth, glslShowDepth)
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

    auto source = nativeVertexOnlySource(mslDepthOnly, hlslDepthOnly, glslDepthOnly);
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
