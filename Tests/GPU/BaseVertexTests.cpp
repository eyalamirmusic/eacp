#include "Common.h"

#include <cstdint>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto insideLeftHalf = 2;
constexpr auto lastLeftColumn = 19;
constexpr auto firstRightColumn = 20;
constexpr auto insideRightHalf = 38;
constexpr auto midRow = 20;

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
        // MSAA would feather the boundary between the halves. Set here, not in
        // the constructor body: the pipeline is built in the initialiser list.
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

    // Both triangles deliberately overhang the target, so each covers its whole
    // half at every row and their shared edge falls between two pixel centres.
    // clang-format off
    static constexpr float halfCoveringTriangles[] = {
        0.f, -3.f,  0.f,  3.f, -4.f, 0.f,  // 0..2: the left half
        0.f,  3.f,  0.f, -3.f,  4.f, 0.f,  // 3..5: the right half
    };
    // clang-format on

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

    check(isGreen(image.at(insideLeftHalf, midRow)));
    check(isGreen(image.at(lastLeftColumn, midRow)));
    check(isRed(image.at(firstRightColumn, midRow)));
    check(isRed(image.at(insideRightHalf, midRow)));

    check(countGreen(image) == 20 * 40);
};

// The x axis carries the assertion: neither backend mirrors horizontally, so
// this assumes nothing about which way up the read-back arrives.
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

    check(isRed(image.at(insideLeftHalf, midRow)));
    check(isRed(image.at(lastLeftColumn, midRow)));
    check(isGreen(image.at(firstRightColumn, midRow)));
    check(isGreen(image.at(insideRightHalf, midRow)));

    check(countGreen(image) == 20 * 40);
};

// Metal's instanced selector previously passed a hardcoded baseVertex of 0.
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
    check(isRed(image.at(insideLeftHalf, midRow)));
    check(isGreen(image.at(insideRightHalf, midRow)));
    check(countGreen(image) == 20 * 40);
};

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
    check(isGreen(image.at(insideLeftHalf, midRow)));
    check(isRed(image.at(insideRightHalf, midRow)));
    check(countGreen(image) == 20 * 40);
};
