#include "Common.h"

#include <cstdint>

// RenderPass::drawIndexed / drawIndexedInstanced's baseVertex, checked by
// rendering off-screen and reading the pixels back.
//
// baseVertex has no CPU-side observable at all -- nothing to query, nothing
// returned -- and it is threaded through two backends by hand, so the only
// honest test is to draw through it and look at what came out. A wrong mapping
// is a silent one: the draw still happens, it just fetches the wrong vertices.
//
// Every case binds the same six-vertex buffer and the same index buffer of
// {0, 1, 2}. Vertices 0..2 make a triangle covering the target's left half,
// 3..5 the same shape mirrored onto the right, so the half that comes back
// green is decided by baseVertex and by nothing else -- neither firstIndex nor
// firstInstance can produce it.
//
// Runs on both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f;
}

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f;
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

ShaderSource shaderSource()
{
    return Platform::isWindows() ? ShaderSource::hlsl(hlslShader)
                                 : ShaderSource::msl(mslShader);
}

struct BaseVertexView final : GPUView
{
    BaseVertexView()
        : library(Device::shared().makeShaderLibrary(
              shaderSource().withVertex("vertexMain").withFragment("fragmentMain")))
        , vertexBuffer(Device::shared().makeBuffer(halfCoveringTriangles))
        , indexBuffer(Device::shared().makeBuffer(firstTriangle, BufferUsage::Index))
        , pipeline(makePipeline())
    {
    }

    RenderPipeline makePipeline()
    {
        // MSAA would feather the boundary between the two halves across a pixel
        // and make the edge assertions ambiguous. Set here rather than in the
        // constructor body for the reason ScissorTests spells out: the pipeline
        // is built in the member initialiser list, so a body call would leave it
        // multisampled while the target is not.
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
        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});

        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);

        if (instanced)
            pass.drawIndexedInstanced(
                indexBuffer, 3, 1, IndexFormat::UInt16, 0, 0, baseVertex);
        else
            pass.drawIndexed(indexBuffer, 3, IndexFormat::UInt16, 0, baseVertex);
    }

    // Both triangles overhang the target far enough that each covers its whole
    // half at every row, so a coverage count is an exact number rather than a
    // rasterisation coin-flip at the corners. Their shared edge sits on x = 0,
    // which falls between two pixel centres at any even width.
    // clang-format off
    static constexpr float halfCoveringTriangles[] = {
        0.f, -3.f,  0.f,  3.f, -4.f, 0.f,  // 0..2: the left half
        0.f,  3.f,  0.f, -3.f,  4.f, 0.f,  // 3..5: the right half
    };
    // clang-format on

    // 16-bit deliberately: this is the width a renderer moves to once it no
    // longer has to bake a vertex offset into the index values, so it is the
    // combination worth proving works.
    static constexpr std::uint16_t firstTriangle[] = {0, 1, 2};

    int baseVertex = 0;
    bool instanced = false;

    ShaderLibrary library;
    Buffer vertexBuffer;
    Buffer indexBuffer;
    RenderPipeline pipeline;
};

int countGreen(const Graphics::Image& image)
{
    auto total = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            if (isGreen(image.at(x, y)))
                ++total;

    return total;
}
} // namespace

// The baseline the rest of the file leans on: at zero, the parameter is not
// there. If this fails, the offset cases prove nothing.
auto tZeroDrawsTheFirstTriangle = test("BaseVertex/zeroDrawsTheFirstTriangle") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = BaseVertexView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(image.width() == 40);

    check(isGreen(image.at(2, 20))); // inside the left half
    check(isGreen(image.at(19, 20))); // its last column
    check(isRed(image.at(20, 20))); // first column of the right half
    check(isRed(image.at(38, 20)));

    check(countGreen(image) == 20 * 40);
};

// The whole point of the parameter. Same buffer, same indices, same everything
// else -- only the fetched vertices move, so the triangle changes sides.
//
// The x axis carries the assertion because neither backend mirrors
// horizontally, so this needs no assumption about which way up the read-back
// arrives.
auto tOffsetSelectsLaterVertices = test("BaseVertex/offsetSelectsLaterVertices") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = BaseVertexView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.baseVertex = 3;

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(image.width() == 40);

    check(isRed(image.at(2, 20))); // the left half is now the clear colour
    check(isRed(image.at(19, 20)));
    check(isGreen(image.at(20, 20))); // and the right half is drawn
    check(isGreen(image.at(38, 20)));

    check(countGreen(image) == 20 * 40);
};

// The instanced path takes the offset in a different argument position on both
// backends, and on Metal it was the one selector already passing a hardcoded
// zero -- exactly the kind of place a parameter gets added to the signature and
// forgotten in the call.
auto tInstancedTakesTheSameOffset =
    test("BaseVertex/instancedTakesTheSameOffset") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = BaseVertexView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.instanced = true;
    view.baseVertex = 3;

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(isRed(image.at(2, 20)));
    check(isGreen(image.at(38, 20)));
    check(countGreen(image) == 20 * 40);
};

// The instanced path with no offset still draws what it did before the
// parameter existed. Its Metal call moved from a hardcoded baseVertex:0 to a
// threaded one, so the defaulted case is a real regression risk rather than a
// tautology.
auto tInstancedZeroIsUnchanged = test("BaseVertex/instancedZeroDrawsTheFirst") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = BaseVertexView {};

    if (!view.pipeline.isValid())
        return;

    view.setBounds({0.f, 0.f, 40.f, 40.f});
    view.instanced = true;

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(isGreen(image.at(2, 20)));
    check(isRed(image.at(38, 20)));
    check(countGreen(image) == 20 * 40);
};
