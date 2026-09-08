#include "Common.h"

// The render side's ranged binds: a storage buffer read from part-way into its
// resource, and the ranges no backend can take - a range naming no buffer, one
// starting before its buffer, one at or past its end - which have to bind
// nothing rather than reach the encoder.
//
// A dropped offset is the bug an eyeball misses: it reads the start of the
// buffer, which is also data, so the draw paints a colour - just not the one
// asked for. Every range here therefore sits past the start of its buffer, and
// the record before it is a different colour.
//
// Everything renders off-screen through View::renderToImage, so it runs in CI on
// both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct Vertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(Vertex, Float2)

namespace
{
constexpr Vertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},

    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

// Two triangles, each covering one half of clip space, so a draw that took the
// wrong geometry or never happened leaves the other half at the clear colour.
constexpr Vertex leftHalf[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 3.f}},
};

constexpr Vertex rightHalf[] = {
    {{0.f, -1.f}},
    {{1.f, -1.f}},
    {{1.f, 3.f}},
};

constexpr std::uint32_t triangle[] = {0, 1, 2};

// Records of four floats, so the offset is a multiple of the widest thing
// either backend aligns a root descriptor to. Record zero is red and record one
// is green: a bind that ignored the range's offset paints red where green was
// asked for.
constexpr float paletteValues[] = {0.9f, 0.1f, 0.1f, 0.f, 0.1f, 0.9f, 0.1f, 0.f};
constexpr auto recordBytes = 4 * (int) sizeof(float);

constexpr auto clearBlue = Graphics::Color {0.f, 0.f, 1.f, 1.f};

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f && c.b < 0.5f;
}

bool isClear(const Graphics::Color& c)
{
    return c.b > 0.5f && c.r < 0.5f && c.g < 0.5f;
}

// Reads its colour out of record zero of whatever range it was bound over.
struct PaletteShader final : ShaderProgram
{
    PaletteShader() { compile(); }

    void define() override
    {
        setPosition(float4(vertexInput(&Vertex::position), 0.f, 1.f));
        setFragment(float4(palette.read3(0u), 1.f));
    }

    Uniform<InputBuffer> palette;

    EACP_SHADER(palette)
};

// Flat colour from a uniform, so two draws of a frame can be told apart.
struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        setPosition(float4(vertexInput(&Vertex::position), 0.f, 1.f));
        setFragment(color);
    }

    Uniform<Float4> color;

    EACP_SHADER(color)
};

struct PaletteView : GPUView
{
    PaletteView()
        : colors(Device::shared(),
                 paletteValues,
                 (int) sizeof(paletteValues),
                 BufferUsage::Storage)
    {
        setSampleCount(1);
        shader.setVertices(fullQuad, 6);
        shader.prepare(sampleCount());
    }

    BufferRange greenRecord() const { return {&colors, recordBytes, recordBytes}; }

    Buffer colors;
    PaletteShader shader;
};

// The bind under test, through the program member: the shader's record zero is
// the buffer's second record.
struct RangedPaletteView final : PaletteView
{
    void render(Frame& frame) override
    {
        shader.palette = greenRecord();

        auto pass = frame.beginPass({clearBlue});
        pass.draw(shader);
    }
};

// The same draw with the unbindable ranges laid over the good one. Each must
// leave the bind before it alone.
struct GuardedPaletteView final : PaletteView
{
    void render(Frame& frame) override
    {
        shader.palette = greenRecord();

        auto pass = frame.beginPass({clearBlue});
        pass.bind(shader);

        const auto slot = shader.palette.slot;

        for (auto bad: {BufferRange {},
                        BufferRange {&colors, -recordBytes, recordBytes},
                        BufferRange {&colors, colors.size(), 0}})
        {
            pass.setVertexStorageBuffer(bad, slot);
            pass.setFragmentStorageBuffer(bad, slot);
        }

        pass.draw(shader.vertexCount());
    }
};

// A good vertex bind with the unbindable ranges over it: what reaches the
// rasterizer is still the triangle bound first.
struct VertexGuardView final : GPUView
{
    VertexGuardView()
        : geometry(Device::shared(),
                   leftHalf,
                   (int) sizeof(leftHalf),
                   BufferUsage::Vertex)
    {
        setSampleCount(1);
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({clearBlue});

        pass.setPipeline(shader.pipeline());
        pass.setVertexBuffer(geometry);

        for (auto bad: {BufferRange {},
                        BufferRange {&geometry, -(int) sizeof(Vertex), 0},
                        BufferRange {&geometry, geometry.size(), 0}})
            pass.setVertexBuffer(bad);

        shader.color = Array {1.f, 0.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.draw(3);
    }

    Buffer geometry;
    FlatShader shader;
};

// The indexed pair: the red half is asked for through range after range that
// names nothing bindable and must never appear, and the green half drawn after
// them says the pass was working.
struct IndexGuardView final : GPUView
{
    IndexGuardView()
        : left(Device::shared(),
               leftHalf,
               (int) sizeof(leftHalf),
               BufferUsage::Vertex)
        , right(Device::shared(),
                rightHalf,
                (int) sizeof(rightHalf),
                BufferUsage::Vertex)
        , indices(
              Device::shared(), triangle, (int) sizeof(triangle), BufferUsage::Index)
    {
        setSampleCount(1);
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({clearBlue});

        pass.bind(shader, right);
        shader.color = Array {1.f, 0.f, 0.f, 1.f};
        pass.setUniforms(shader);

        for (auto bad: {BufferRange {},
                        BufferRange {&indices, -(int) sizeof(std::uint32_t), 0},
                        BufferRange {&indices, indices.size(), 0}})
        {
            pass.drawIndexed(bad, 3);
            pass.drawIndexedInstanced(bad, 3, 1);
        }

        pass.bind(shader, left);
        shader.color = Array {0.f, 1.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.drawIndexed(indices, 3);
    }

    Buffer left;
    Buffer right;
    Buffer indices;
    FlatShader shader;
};

template <typename View>
bool ready(View& view)
{
    if (!Device::shared().isValid() || !view.shader.pipeline().isValid())
        return false;

    view.setBounds({0.f, 0.f, 32.f, 32.f});
    return true;
}
} // namespace

// A storage buffer bound at an offset: the shader's record zero is the record
// at the offset, and reading the one before it would be red.
auto tStorageBufferBoundAtOffset =
    test("RenderRanges/storageBufferReadsFromTheOffset") = []
{
    auto view = RangedPaletteView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(isGreen(image.at(16, 16)), "the record at the offset, not the first one");
};

auto tStorageGuardsLeaveTheBind =
    test("RenderRanges/anUnbindableStorageRangeBindsNothing") = []
{
    auto view = GuardedPaletteView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(isGreen(image.at(16, 16)), "the good bind survived the bad ones");
};

auto tVertexGuardsLeaveTheBind =
    test("RenderRanges/anUnbindableVertexRangeBindsNothing") = []
{
    auto view = VertexGuardView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(isRed(image.at(8, 16)), "the triangle bound first is the one drawn");
    check(isClear(image.at(24, 16)), "and nothing else was");
};

auto tIndexGuardsDrawNothing =
    test("RenderRanges/anUnbindableIndexRangeDrawsNothing") = []
{
    auto view = IndexGuardView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(isClear(image.at(24, 16)), "no draw took an unbindable index range");
    check(isGreen(image.at(8, 16)), "and the draw after them still happened");
};
