#include "Common.h"

// GPU::StreamingBuffers, drawn: that a slice bound at its offset draws the
// geometry that was written there.
//
// The other half of StreamingBufferTests, which checks where the slices are
// without ever binding one. What this catches is the bug an eyeball misses: a
// bind that ignores the range's offset reads from the start of the arena, and
// the start of the arena is *also* geometry, so it draws a picture - just not
// the one asked for. Every slice here is therefore pushed off the start of its
// arena by a filler write first, and the check is that each draw painted its
// own half and nothing painted the other.
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
// Two triangles, each covering one half of clip space, so a draw that took the
// wrong slice paints the wrong half and the right half keeps the clear colour.
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

constexpr Vertex bothHalves[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 3.f}},

    {{0.f, -1.f}},
    {{1.f, -1.f}},
    {{1.f, 3.f}},
};

// What sits at the start of every arena: a triangle with no area, well off
// screen. A draw that read from offset zero would draw this and paint nothing,
// which is what leaves the half it should have painted at the clear colour.
constexpr Vertex filler[] = {
    {{9.f, 9.f}},
    {{9.f, 9.f}},
    {{9.f, 9.f}},
};

constexpr std::uint32_t triangle[] = {0, 1, 2};
constexpr std::uint32_t twoTriangles[] = {0, 1, 2, 3, 4, 5};
constexpr std::uint32_t fillerIndices[] = {0, 0, 0};

// Flat colour from a uniform, so the two draws of a frame can be told apart.
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

constexpr auto clearBlue = Graphics::Color {0.f, 0.f, 1.f, 1.f};

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f && c.b < 0.5f;
}

// A view whose geometry is streamed every render rather than kept: two
// streams, one per usage, exactly the shape a per-draw renderer has.
struct StreamedView : GPUView
{
    StreamedView()
        : vertices(BufferUsage::Vertex)
        , indices(BufferUsage::Index)
    {
        setSampleCount(1);
        shader.prepare(sampleCount(), false);
    }

    // Pushes the arenas' starts out of the way, so nothing that follows can
    // land at offset zero by accident.
    void writeFillers()
    {
        vertices.write(filler, sizeof(filler));
        indices.write(fillerIndices, sizeof(fillerIndices));
    }

    // Whether every slice a render bound sat past the start of its arena,
    // which is what makes the picture proof that the offset was used.
    void note(const BufferRange& range)
    {
        if (range.offset == 0)
            offsetsWereNonZero = false;
    }

    FlatShader shader;
    StreamingBuffers vertices;
    StreamingBuffers indices;
    bool offsetsWereNonZero = true;
};

// Each half streamed as its own slice: the first bound through bind(program,
// range), the second through setVertexBuffer(range), both drawn through one
// index slice.
struct TwoSliceView final : StreamedView
{
    void render(Frame& frame) override
    {
        writeFillers();

        const auto left = vertices.write(leftHalf, sizeof(leftHalf));
        const auto right = vertices.write(rightHalf, sizeof(rightHalf));
        const auto tri = indices.write(triangle, sizeof(triangle));

        note(left);
        note(right);
        note(tri);

        auto pass = frame.beginPass({clearBlue});

        pass.bind(shader, left);
        shader.color = Array {1.f, 0.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.drawIndexed(tri, 3);

        pass.setVertexBuffer(right);
        shader.color = Array {0.f, 1.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.drawIndexed(tri, 3);
    }
};

// One vertex slice holding both halves and one index slice holding both
// triangles. firstIndex counts from the slice's own start: the second triangle
// is at index 3 *of the slice*, which is not index 3 of the arena.
struct FirstIndexView final : StreamedView
{
    void render(Frame& frame) override
    {
        writeFillers();

        const auto both = vertices.write(bothHalves, sizeof(bothHalves));
        const auto tris = indices.write(twoTriangles, sizeof(twoTriangles));

        note(both);
        note(tris);

        auto pass = frame.beginPass({clearBlue});

        pass.bind(shader, both);
        shader.color = Array {1.f, 0.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.drawIndexed(tris, 3, IndexFormat::UInt32, 0);

        shader.color = Array {0.f, 1.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.drawIndexed(tris, 3, IndexFormat::UInt32, 3);
    }
};

// The same through the instanced entry point, which is its own code on both
// backends and could add the offset in on one and forget it on the other.
struct InstancedSliceView final : StreamedView
{
    void render(Frame& frame) override
    {
        writeFillers();

        const auto both = vertices.write(bothHalves, sizeof(bothHalves));
        const auto tris = indices.write(twoTriangles, sizeof(twoTriangles));

        note(both);
        note(tris);

        auto pass = frame.beginPass({clearBlue});

        pass.bind(shader, both);
        shader.color = Array {1.f, 0.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.drawIndexedInstanced(tris, 3, 1, IndexFormat::UInt32, 0);

        shader.color = Array {0.f, 1.f, 0.f, 1.f};
        pass.setUniforms(shader);
        pass.drawIndexedInstanced(tris, 3, 1, IndexFormat::UInt32, 3);
    }
};

template <typename View>
bool ready(View& view)
{
    if (!Device::shared().isValid() || !view.shader.pipeline().isValid())
        return false;

    view.setBounds({0.f, 0.f, 32.f, 32.f});
    return true;
}

template <typename View>
void checkBothHalves(View& view, const Graphics::Image& image)
{
    check(image.isValid());
    check(view.offsetsWereNonZero, "every slice sat past the start of its arena");
    check(isRed(image.at(8, 16)), "the left half came from its slice");
    check(isGreen(image.at(24, 16)), "and the right half from its slice");
}
} // namespace

auto tTwoSlices = test("StreamedRanges/slicesDrawWhatWasWrittenThere") = []
{
    auto view = TwoSliceView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);
    checkBothHalves(view, image);
};

auto tFirstIndex = test("StreamedRanges/firstIndexCountsFromTheSlice") = []
{
    auto view = FirstIndexView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);
    checkBothHalves(view, image);
};

auto tInstanced = test("StreamedRanges/instancedDrawTakesTheSlice") = []
{
    auto view = InstancedSliceView {};

    if (!ready(view))
        return;

    auto image = view.renderToImage(1.f);
    checkBothHalves(view, image);
};

// Rendered enough times for every pool to have come round: the slices then
// come out of an arena a previous frame filled, at the same offsets, and the
// picture has to be the same picture.
auto tRecycledArena = test("StreamedRanges/recycledArenaDrawsTheSame") = []
{
    auto view = TwoSliceView {};

    if (!ready(view))
        return;

    auto image = Graphics::Image {};

    for (auto frame = 0; frame < 2 * StreamingBuffers::framesInFlight; ++frame)
        image = view.renderToImage(1.f);

    check(view.vertices.bufferCount() == StreamingBuffers::framesInFlight,
          "one arena per frame in flight, and no more");
    checkBothHalves(view, image);
};
