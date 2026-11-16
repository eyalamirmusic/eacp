#include "Common.h"

// RenderPass::setVertexTexture and setVertexDepthTexture, and the texture slot
// count past eight: a vertex shader sampling the way bgfx's terrain, RSM and
// tessellation examples do. The shaders are hand-written, the way an imported
// one is, and every picture is checked against the CPU's prediction of it.
//
// The drawing cases self-skip without a device and where the device reports
// !supportsPerBindSamplers(), whose per-bind registers hold a fixed sampler.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace VertexTextureTests
{
namespace
{
constexpr auto columns = 16;
constexpr auto gridHeight = 16;
constexpr auto depthSceneHeight = 4;

struct ColumnVertex
{
    float x;
    float u;
    float top;
};

struct PositionVertex
{
    float position[2];
};

constexpr PositionVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

constexpr PositionVertex leftHalf[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},
};

// How many rows, from the bottom, column `column` of the height map raises:
// every count from 1 to 16 once, in an order no interpolation would give.
int rowsRaised(int column)
{
    return (column * 5) % columns + 1;
}

Vector<float> makeHeights()
{
    auto heights = Vector<float> {};

    for (auto column = 0; column < columns; ++column)
        heights.add((float) rowsRaised(column) / (float) gridHeight);

    return heights;
}

// Six vertices a column: the bottom pair at y = -1 and the top pair raised by
// whatever the vertex shader samples for the column's u.
Vector<ColumnVertex> makeColumnGrid()
{
    auto vertices = Vector<ColumnVertex> {};

    for (auto column = 0; column < columns; ++column)
    {
        const auto left = -1.f + 2.f * (float) column / (float) columns;
        const auto right = -1.f + 2.f * (float) (column + 1) / (float) columns;
        const auto u = ((float) column + 0.5f) / (float) columns;

        for (auto corner: {ColumnVertex {left, u, 0.f},
                           ColumnVertex {right, u, 0.f},
                           ColumnVertex {left, u, 1.f},
                           ColumnVertex {right, u, 0.f},
                           ColumnVertex {right, u, 1.f},
                           ColumnVertex {left, u, 1.f}})
            vertices.add(corner);
    }

    return vertices;
}

RenderPipelineDescriptor describeColumns(const ShaderLibrary& library,
                                         int sampleCount)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.sampleCount = sampleCount;
    descriptor.vertexLayout.attribute(VertexFormat::Float3, 0);
    descriptor.vertexLayout.stride = sizeof(ColumnVertex);
    return descriptor;
}

ShaderLibrary makeLibrary(ShaderSource source)
{
    return Device::shared().makeShaderLibrary(
        source.withVertex("vertexMain").withFragment("fragmentMain"));
}

TextureDescriptor describeHeightMap()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = columns;
    descriptor.height = 1;
    descriptor.format = TextureFormat::R32Float;
    return descriptor;
}

TextureDescriptor describeDepthScene()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = columns;
    descriptor.height = depthSceneHeight;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    descriptor.sampleableDepth = true;
    return descriptor;
}

Texture makeOnePixel(std::uint32_t rgba)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = 1;
    descriptor.height = 1;
    descriptor.format = TextureFormat::RGBA8Unorm;
    return Device::shared().makeTexture(descriptor, &rgba);
}

bool canDraw()
{
    return Device::shared().isValid() && Device::shared().supportsPerBindSamplers()
           && Device::shared().supportsVertexTextures();
}

Graphics::Image renderAt(GPUView& view, int width, int height)
{
    view.setBounds({0.f, 0.f, (float) width, (float) height});
    return view.renderToImage(1.f);
}

// Pixels that disagree with the columns rising rowsFor(column) rows from the
// bottom, white over black.
template <typename RowsFor>
int countColumnMismatches(const Graphics::Image& image, RowsFor rowsFor)
{
    auto mismatches = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
        {
            const auto rowFromBottom = image.height() - 1 - y;
            const auto lit = rowFromBottom < rowsFor(x);
            const auto colour = image.at(x, y);
            const auto bright = colour.r > 0.9f && colour.g > 0.9f;
            const auto dark = colour.r < 0.1f && colour.g < 0.1f;

            if (lit ? !bright : !dark)
                ++mismatches;
        }

    return mismatches;
}

struct HeightMapView final : GPUView
{
    HeightMapView()
        : heights(makeHeights())
        , heightMap(
              Device::shared().makeTexture(describeHeightMap(), heights.data()))
        , library(makeLibrary(nativeShaderSource("VertexTexture/heights")))
        , pipeline(makePipeline())
        , grid(makeColumnGrid())
        , vertices(Device::shared().makeBuffer(
              grid.data(), (std::int64_t) (grid.size() * sizeof(ColumnVertex))))
        , sampler(Device::shared().makeSampler({}))
    {
    }

    RenderPipeline makePipeline()
    {
        setSampleCount(1);
        return Device::shared().makeRenderPipeline(
            describeColumns(library, sampleCount()));
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color::black()});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertices);
        pass.setVertexTexture(heightMap, 0, sampler);
        pass.draw((int) grid.size());
    }

    bool isReady() const
    {
        return heightMap.isValid() && pipeline.isValid() && sampler.isValid();
    }

    Vector<float> heights;
    Texture heightMap;
    ShaderLibrary library;
    RenderPipeline pipeline;
    Vector<ColumnVertex> grid;
    Buffer vertices;
    Sampler sampler;
};

// Depth 0.25 over the left half of a target, then columns raised to the full
// height where a Less comparison against 0.5 passes and to half where not.
struct DepthCompareView final : GPUView
{
    DepthCompareView()
        : scene(Device::shared().makeTexture(describeDepthScene()))
        , writerLibrary(Device::shared().makeShaderLibrary(
              nativeVertexOnlySource("VertexTexture/depthWriter")))
        , writer(makeWriter())
        , library(makeLibrary(nativeShaderSource("VertexTexture/compareHeights")))
        , pipeline(makePipeline())
        , grid(makeColumnGrid())
        , vertices(Device::shared().makeBuffer(
              grid.data(), (std::int64_t) (grid.size() * sizeof(ColumnVertex))))
        , left(Device::shared().makeBuffer(leftHalf))
        , sampler(Device::shared().makeSampler(describeLess()))
    {
    }

    static SamplerDescriptor describeLess()
    {
        auto descriptor = SamplerDescriptor {};
        descriptor.compare = CompareFunction::Less;
        return descriptor;
    }

    RenderPipeline makeWriter()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &writerLibrary;
        descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
        descriptor.depth = true;
        descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
        descriptor.vertexLayout.stride = sizeof(PositionVertex);
        return Device::shared().makeRenderPipeline(descriptor);
    }

    RenderPipeline makePipeline()
    {
        setSampleCount(1);
        return Device::shared().makeRenderPipeline(
            describeColumns(library, sampleCount()));
    }

    void render(Frame& frame) override
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.depthAction = DepthAction::Keep;

            auto into = frame.beginPass(scene, descriptor);
            into.setPipeline(writer);
            into.setVertexBuffer(left);
            into.draw(6);
        }

        auto pass = frame.beginPass({Graphics::Color::black()});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertices);
        pass.setVertexDepthTexture(scene, 0, sampler);
        pass.draw((int) grid.size());
    }

    bool isReady() const
    {
        return scene.hasSampleableDepth() && writer.isValid() && pipeline.isValid()
               && sampler.isValid();
    }

    Texture scene;
    ShaderLibrary writerLibrary;
    RenderPipeline writer;
    ShaderLibrary library;
    RenderPipeline pipeline;
    Vector<ColumnVertex> grid;
    Buffer vertices;
    Buffer left;
    Sampler sampler;
};

// Red at the vertex stage's slot 12 and green at the fragment stage's, summed:
// yellow only if the two stages kept two textures at one slot number. Only
// built where the stages keep their own texture slots, which a WebGPU device
// does not, so its shader has no WGSL twin.
struct TwoStagesView final : GPUView
{
    TwoStagesView()
        : red(makeOnePixel(0xff0000ff))
        , green(makeOnePixel(0xff00ff00))
        , library(makeLibrary(
              ShaderSource::msl(testShaderFile("VertexTexture/twoStages.metal"))))
        , pipeline(makePipeline())
        , vertices(Device::shared().makeBuffer(fullQuad))
        , sampler(Device::shared().makeSampler({}))
    {
    }

    RenderPipeline makePipeline()
    {
        setSampleCount(1);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
        descriptor.vertexLayout.stride = sizeof(PositionVertex);
        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color::black()});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertices);
        pass.setVertexTexture(red, 12, sampler);
        pass.setFragmentTexture(green, 12, sampler);
        pass.draw(6);
    }

    bool isReady() const
    {
        return red.isValid() && green.isValid() && pipeline.isValid()
               && sampler.isValid();
    }

    Texture red;
    Texture green;
    ShaderLibrary library;
    RenderPipeline pipeline;
    Buffer vertices;
    Sampler sampler;
};
} // namespace

auto tSlotsReported = test("VertexTexture/theDeviceReportsItsTextureSlots") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
    {
        check(device.maxTextureSlots() == 0);
        check(!device.supportsVertexTextures());
        return;
    }

    const auto slots = device.maxTextureSlots();

    check(device.supportsVertexTextures());
    check(slots >= maxTextureSlots);
    check(slots <= RenderPass::maxStageTextureSlots);

    if constexpr (Platform::isApple())
    {
        check(slots == RenderPass::maxStageTextureSlots);
        check(!device.stagesShareTextureSlots());
    }
    else if constexpr (Platform::isWindows())
    {
        check(slots == maxTextureSlots);
        check(device.stagesShareTextureSlots());
    }
    else
    {
        check(device.stagesShareTextureSlots());
    }
};

auto tHeightMap = test("VertexTexture/aHeightMapRaisesTheGridItDisplaces") = []
{
    if (!canDraw())
        return;

    auto view = HeightMapView {};

    if (!view.isReady())
        return;

    auto image = renderAt(view, columns, gridHeight);

    check(image.isValid() && image.width() == columns);
    check(countColumnMismatches(image, [](int x) { return rowsRaised(x); }) == 0);
};

auto tDepthCompare =
    test("VertexTexture/aComparisonSamplerReadsDepthInTheVertexStage") = []
{
    if (!canDraw())
        return;

    auto view = DepthCompareView {};

    if (!view.isReady())
        return;

    auto image = renderAt(view, columns, gridHeight);

    check(image.isValid() && image.width() == columns);

    const auto rowsFor = [](int x)
    { return x < columns / 2 ? gridHeight / 2 : gridHeight; };

    check(countColumnMismatches(image, rowsFor) == 0);
};

auto tSlotTwelve = test("VertexTexture/eachStageKeepsItsOwnSlotTwelve") = []
{
    if (!canDraw() || Device::shared().stagesShareTextureSlots()
        || Device::shared().maxTextureSlots() <= 12)
        return;

    auto view = TwoStagesView {};

    check(view.isReady());

    auto image = renderAt(view, 4, 4);

    check(image.isValid());

    auto mismatches = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
        {
            const auto colour = image.at(x, y);

            if (colour.r < 0.9f || colour.g < 0.9f || colour.b > 0.1f)
                ++mismatches;
        }

    check(mismatches == 0);
};
} // namespace VertexTextureTests
