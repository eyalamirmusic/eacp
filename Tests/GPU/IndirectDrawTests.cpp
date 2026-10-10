#include "Common.h"

#include <cstdint>
#include <functional>

// RenderPass::drawIndirect and drawIndexedIndirect, with the argument records
// written by a kernel earlier in the same frame and the pixels read back.
//
// The geometry is BaseVertexTests': six vertices, 0..2 a triangle covering the
// left half of the target and 3..5 the right half, so which half comes back
// green says which vertices a record fetched, and nothing else can produce it.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto targetSize = 40;
constexpr auto halfPixels = targetSize / 2 * targetSize;
constexpr auto argumentWords = 32;
constexpr auto bgfxStride = std::int64_t {32};

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f;
}

const char* mslShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { float2 position [[attribute(0)]]; };

vertex float4 vertexMain(VertexIn in [[stage_in]])
{
    return float4(in.position, 0.0, 1.0);
}

fragment float4 fragmentMain() { return float4(0.0, 1.0, 0.0, 1.0); }
)";

const char* hlslShader = R"(
struct VertexIn { float2 position : TEXCOORD0; };
struct VertexOut { float4 position : SV_Position; };

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 0.0, 1.0);
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }
)";

const char* glslShader = R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
#endif
)";

struct CopyWordsKernel final : ComputeProgram
{
    CopyWordsKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i]);
    }

    Uniform<UIntInputBuffer> input;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(input, output)
};

// clang-format off
constexpr float halfCoveringTriangles[] = {
    0.f, -3.f,  0.f,  3.f, -4.f, 0.f,
    0.f,  3.f,  0.f, -3.f,  4.f, 0.f,
};
// clang-format on

constexpr std::uint16_t allSixIndices[] = {0, 1, 2, 3, 4, 5};

using DrawCall = std::function<void(RenderPass&, const Buffer&, const Buffer&)>;

Buffer makeWordBuffer(const Vector<std::uint32_t>& words)
{
    auto padded = Vector<std::uint32_t> {};
    padded.assign(argumentWords, 0u);

    for (auto i = 0; i < words.size() && i < argumentWords; ++i)
        padded[i] = words[i];

    return Buffer {Device::shared(),
                   padded.data(),
                   (std::int64_t) sizeof(std::uint32_t) * argumentWords,
                   BufferUsage::Storage};
}

struct IndirectDrawView final : GPUView
{
    IndirectDrawView(const Vector<std::uint32_t>& words, DrawCall callToIssue)
        : library(Device::shared().makeShaderLibrary(
              nativeShaderSource(mslShader, hlslShader, glslShader)
                  .withVertex("vertexMain")
                  .withFragment("fragmentMain")))
        , vertices(Device::shared().makeBuffer(halfCoveringTriangles))
        , indices(Device::shared().makeBuffer(allSixIndices, BufferUsage::Index))
        , source(makeWordBuffer(words))
        , arguments(Device::shared(),
                    nullptr,
                    (std::int64_t) sizeof(std::uint32_t) * argumentWords,
                    BufferUsage::Storage)
        , pipeline(makePipeline())
        , issue(std::move(callToIssue))
    {
        copy.input = source;
        copy.output = arguments;
        copy.prepare();
    }

    RenderPipeline makePipeline()
    {
        setSampleCount(1);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
        descriptor.vertexLayout.stride = sizeof(float) * 2;

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        {
            auto compute = frame.beginCompute();
            compute.dispatch(copy, argumentWords);
        }

        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertices);
        issue(pass, arguments, indices);
    }

    ShaderLibrary library;
    Buffer vertices;
    Buffer indices;
    Buffer source;
    Buffer arguments;
    RenderPipeline pipeline;
    CopyWordsKernel copy;
    DrawCall issue = [](RenderPass&, const Buffer&, const Buffer&) {};
};

Graphics::Image render(const Vector<std::uint32_t>& words, DrawCall issue)
{
    auto view = IndirectDrawView {words, std::move(issue)};

    if (!view.pipeline.isValid())
        return {};

    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    return view.renderToImage(1.f);
}

int countGreen(const Graphics::Image& image)
{
    auto total = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            if (isGreen(image.at(x, y)))
                ++total;

    return total;
}

bool leftHalfOnly(const Graphics::Image& image)
{
    return isGreen(image.at(2, 20)) && !isGreen(image.at(38, 20))
           && countGreen(image) == halfPixels;
}

bool rightHalfOnly(const Graphics::Image& image)
{
    return !isGreen(image.at(2, 20)) && isGreen(image.at(38, 20))
           && countGreen(image) == halfPixels;
}

bool sameColor(const Graphics::Color& a, const Graphics::Color& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool samePixels(const Graphics::Image& a, const Graphics::Image& b)
{
    if (a.width() != b.width() || a.height() != b.height())
        return false;

    for (auto y = 0; y < a.height(); ++y)
        for (auto x = 0; x < a.width(); ++x)
            if (!sameColor(a.at(x, y), b.at(x, y)))
                return false;

    return true;
}

DrawCall drawIndirectAt(std::int64_t offset)
{
    return [offset](RenderPass& pass, const Buffer& arguments, const Buffer&)
    { pass.drawIndirect(arguments, offset); };
}

DrawCall drawIndirectRun(int drawCount, std::int64_t stride)
{
    return [=](RenderPass& pass, const Buffer& arguments, const Buffer&)
    { pass.drawIndirect(arguments, 0, drawCount, stride); };
}

DrawCall drawIndexedIndirectAt(std::int64_t offset)
{
    return [offset](RenderPass& pass, const Buffer& arguments, const Buffer& indices)
    { pass.drawIndexedIndirect(indices, IndexFormat::UInt16, arguments, offset); };
}

bool hasDevice()
{
    return Device::shared().isValid();
}
} // namespace

auto tIndirectDrawMatchesDirectDraw = test("IndirectDraw/matchesTheDirectDraw") = []
{
    if (!hasDevice())
        return;

    auto indirect = render({3, 1, 3, 0}, drawIndirectAt(0));
    auto direct = render(
        {}, [](RenderPass& pass, const Buffer&, const Buffer&) { pass.draw(3, 3); });

    check(indirect.isValid());
    check(rightHalfOnly(indirect));
    check(samePixels(indirect, direct));
};

auto tIndirectDrawReadsAtOffset = test("IndirectDraw/readsTheRecordAtItsOffset") = []
{
    if (!hasDevice())
        return;

    auto image = render({3, 1, 3, 0, 3, 1, 0, 0}, drawIndirectAt(16));

    check(leftHalfOnly(image));
};

auto tIndirectDrawIndexedMatchesDirect =
    test("IndirectDraw/indexedMatchesTheDirectDraw") = []
{
    if (!hasDevice())
        return;

    auto indirect = render({3, 1, 3, 0, 0}, drawIndexedIndirectAt(0));
    auto direct = render({},
                         [](RenderPass& pass, const Buffer&, const Buffer& indices)
                         { pass.drawIndexed(indices, 3, IndexFormat::UInt16, 3); });

    check(rightHalfOnly(indirect));
    check(samePixels(indirect, direct));
};

auto tIndirectDrawIndexedBaseVertex =
    test("IndirectDraw/indexedTakesItsBaseVertex") = []
{
    if (!hasDevice())
        return;

    auto image = render({3, 1, 0, 3, 0}, drawIndexedIndirectAt(0));

    check(rightHalfOnly(image));
};

auto tIndirectDrawMultiDraw = test("IndirectDraw/multiDrawAtBgfxStride") = []
{
    if (!hasDevice())
        return;

    auto words = Vector<std::uint32_t> {};
    words.assign(16, 0u);
    words[0] = 3;
    words[1] = 1;
    words[8] = 3;
    words[9] = 1;
    words[10] = 3;

    auto image = render(words, drawIndirectRun(2, bgfxStride));

    check(countGreen(image) == targetSize * targetSize);
};

auto tIndirectDrawIndexedMultiDraw = test("IndirectDraw/indexedMultiDrawPacked") = []
{
    if (!hasDevice())
        return;

    auto image = render(
        {3, 1, 0, 0, 0, 3, 1, 3, 0, 0},
        [](RenderPass& pass, const Buffer& arguments, const Buffer& indices)
        {
            pass.drawIndexedIndirect(
                BufferRange::of(indices), IndexFormat::UInt16, arguments, 0, 2);
        });

    check(countGreen(image) == targetSize * targetSize);
};

auto tIndirectDrawZeroInstances = test("IndirectDraw/zeroInstancesDrawNothing") = []
{
    if (!hasDevice())
        return;

    check(countGreen(render({3, 0, 0, 0}, drawIndirectAt(0))) == 0);
    check(countGreen(render({3, 0, 0, 0, 0}, drawIndexedIndirectAt(0))) == 0);

    auto words = Vector<std::uint32_t> {3, 0, 3, 0, 3, 1, 0, 0};
    check(leftHalfOnly(render(words, drawIndirectRun(2, 0))));
};

auto tIndirectDrawRefusals =
    test("IndirectDraw/badOffsetsAndStridesDrawNothing") = []
{
    if (!hasDevice())
        return;

    auto words = Vector<std::uint32_t> {3, 1, 0, 0};
    auto pastTheEnd = (std::int64_t) sizeof(std::uint32_t) * argumentWords - 12;

    check(countGreen(render(words, drawIndirectAt(2))) == 0);
    check(countGreen(render(words, drawIndirectAt(-4))) == 0);
    check(countGreen(render(words, drawIndirectAt(pastTheEnd))) == 0);
    check(countGreen(render(words, drawIndirectRun(2, 12))) == 0);
    check(countGreen(render(words, drawIndirectRun(5, bgfxStride))) == 0);
    check(countGreen(render(words, drawIndirectRun(0, 0))) == 0);
};

auto tIndirectDrawIndirectCount = test("IndirectDraw/countFollowsTheDevice") = []
{
    if (!hasDevice())
        return;

    auto supported = Device::shared().supportsIndirectCount();

    if constexpr (Platform::isApple())
        check(!supported);

    auto words = Vector<std::uint32_t> {3, 1, 0, 0, 3, 1, 3, 0, 1};
    auto image = render(words,
                        [](RenderPass& pass, const Buffer& arguments, const Buffer&)
                        { pass.drawIndirectCount(arguments, 0, arguments, 32, 2); });

    if (supported)
        check(leftHalfOnly(image));
    else
        check(countGreen(image) == 0);
};
