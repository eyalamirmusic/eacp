#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

// GPU::StreamingBuffers under the load it was rebuilt for: a renderer that
// streams every draw's geometry, per draw, every frame.
//
// That is what a game renderer does until its vertex cache lives on the GPU.
// Doom 3 on eacp keeps that cache in system memory and hands each surface over
// at the draw that needs it - so a level is not a handful of buffers bound in
// turn but some fifteen hundred writes a frame, from a few hundred bytes to a
// few kilobytes, each bound and drawn before the next is written. This app is
// that shape with the artistry removed: two thousand small n-gons, each its own
// vertices.write() + indices.write() + bind + drawIndexed, every frame.
//
// The old StreamingBuffers gave every write a GPU buffer of its own, and under
// this load that design fails twice over. Two thousand writes is two thousand
// MTLBuffers, every one of which has to be made resident before the command
// buffer can be scheduled. And a buffer was reused by *position* - the seventh
// write of a frame got the seventh buffer - so it was sized for the largest
// thing that had ever landed seventh. Change the draw order and a large mesh
// lands in a slot sized for a small one and the buffer is reallocated; a
// renderer sorting its draws by material re-sorts them whenever the camera
// moves, so after a level load this kept reallocating for *seconds*.
//
// The arena has neither problem: one buffer per frame in flight, a memcpy and a
// cursor bump per write, and nothing allocated at all once the arena is as big
// as the biggest frame. Which is a claim with a number attached -
// Device::buffersCreated() - and the number is what this app prints.
//
// So the two things the app does that a demo would not bother with:
//
//   the draw order is shuffled every frame.  Slot k of this frame holds a
//   different mesh - and therefore a different byte count - than slot k of the
//   last, which is precisely the case the per-position design got wrong. Under
//   the arena it is not a case at all: a slice is its own bytes rounded up to
//   an alignment, wherever in the frame it was taken.
//
//   the load doubles for one second in every three.  A level load, a portal
//   opening, a room full of particles: the frame's byte total jumps, the arenas
//   have to grow *while draws already recorded are pointing into them*, and one
//   period later the extra arenas are folded back into one. That fold is what
//   keeps steady state at one buffer per pool, and it only runs after a frame
//   has outgrown itself - so a stress test that never grows never tests it.
//
// The numbers, printed once a second: `draws` this frame, one bind and one
// drawIndexed each; `streamed`, the bytes those meshes asked write() to copy
// (the arenas hold more - a slice starts on a 256-byte boundary and most of
// these meshes are 80-260 bytes, so the padding turns up in `reserved`);
// `created`, **the** number, GPU buffers made in the last second, zero at
// steady state and moving only for the surge; `arenas`, bufferCount() of both
// streams, six at rest; `reserved`, what those arenas hold; and `cpu` / `gpu`,
// milliseconds in render() against what the GPU spent on the pass.
//
// Run with --check to do the same headlessly for fourteen frames - with the
// load doubled over frames 6 to 8, so growth and the fold both happen - and
// print the per-frame table plus the assertions: nothing allocated at steady
// state, six arenas at the end, and a picture that actually has meshes in it.
// It exits non-zero if any of that fails.

using namespace eacp;
using namespace GPU;

namespace
{
constexpr auto pi = 3.14159265358979f;

// A little over Doom 3's per-frame draw count, which is what made the per-write
// design's cost visible in the first place.
constexpr auto defaultMeshCount = 2000;

// Fan-triangulated n-gons, and the range matters more than the shapes: a mesh
// is (sides + 1) vertices and 3 * sides indices, so the per-draw byte count
// varies by better than 3x across the grid. A stress test where every write is
// the same size would not notice a slot sized for the wrong one.
constexpr auto minSides = 3;
constexpr auto maxSides = 12;

constexpr auto maxVertices = maxSides + 1;
constexpr auto maxIndices = maxSides * 3;

// Long enough that the surge frames settle at their own size, and far enough
// apart that the frames between them are unambiguously steady state.
constexpr auto surgePeriod = 3.f;
constexpr auto surgeLength = 1.f;

constexpr auto backgroundColor = Graphics::Color {0.04f, 0.05f, 0.07f};

// Position and colour, twenty bytes - deliberately small, because the ratio
// that matters is the mesh's bytes against StreamingBuffers::alignment. Most of
// these fit in one 256-byte slice and the biggest need two, which is the
// padding `reserved` reports and a fat vertex would hide.
struct MeshVertex
{
    float position[2];
    float color[3];
};

// Everything is laid out in a square space - y in [-1, 1], x in [-aspect,
// aspect] - so an n-gon built from a circle is round rather than an ellipse.
// viewScale is the one multiplication taking that back to clip space, and the
// only thing in this shader that is not per-vertex.
struct MeshShader final : ShaderProgram
{
    MeshShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&MeshVertex::position);
        auto color = vertexInput(&MeshVertex::color);

        setPosition(float4(position * viewScale, 0.f, 1.f));
        setFragment(float4(varying(color), 1.f));
    }

    Uniform<Float2> viewScale;

    EACP_SHADER(viewScale)
};

struct Vec2
{
    float x = 0.f;
    float y = 0.f;
};

// The grid every mesh gets a cell of. Sized from the count, so the surge does
// not merely draw twice as much - it re-lays-out the picture, which is what a
// scene change looks like from the buffers' point of view.
struct Layout
{
    int columns = 1;
    int rows = 1;
    float cellWidth = 0.f;
    float cellHeight = 0.f;
    float radius = 0.f;
};

Layout layoutFor(int count, float aspect)
{
    auto layout = Layout {};

    layout.columns = std::max(1, (int) std::ceil(std::sqrt((float) count * aspect)));
    layout.rows = std::max(1, (count + layout.columns - 1) / layout.columns);
    layout.cellWidth = 2.f * aspect / (float) layout.columns;
    layout.cellHeight = 2.f / (float) layout.rows;
    layout.radius = 0.42f * std::min(layout.cellWidth, layout.cellHeight);

    return layout;
}

// Where mesh `id` lives - a function of the id alone and *not* of when it is
// drawn. That is the point of the shuffle: the order changes every frame and
// the picture does not, so anything that does move is a slice read at the wrong
// offset rather than an artefact of the test.
Vec2 cellCentre(const Layout& layout, int id)
{
    const auto column = id % layout.columns;
    const auto row = id / layout.columns;
    return {(float) layout.columns * layout.cellWidth * -0.5f
                + layout.cellWidth * ((float) column + 0.5f),
            -1.f + layout.cellHeight * ((float) row + 0.5f)};
}

// Times seven so neighbouring cells differ, which spreads the byte sizes over
// the grid instead of banding them into rows.
int sidesFor(int id)
{
    return minSides + (id * 7) % (maxSides - minSides + 1);
}

// A hue wheel as three cosines a third of a turn apart, lifted well clear of
// the background. The lift is not cosmetic: --check decides a cell was drawn by
// its luminance, and a pure blue n-gon is darker than plenty of backgrounds.
Graphics::Color colorFor(int id)
{
    const auto hue = (float) id * 0.017f;
    const auto channel = [hue](float phase)
    { return 0.32f + 0.68f * (0.5f + 0.5f * std::cos(hue + phase)); };

    return {channel(0.f), channel(2.f * pi / 3.f), channel(4.f * pi / 3.f)};
}

// One mesh into the scratch vectors: a bright centre and a darker rim, spun and
// pulsed by its own id so the picture is obviously live. The vectors are the
// caller's and are cleared rather than rebuilt - two thousand of these happen
// per frame, and a malloc each would be what the profile showed.
void buildMesh(std::vector<MeshVertex>& vertices,
               std::vector<std::uint32_t>& indices,
               const Layout& layout,
               int id,
               float time)
{
    const auto sides = sidesFor(id);
    const auto centre = cellCentre(layout, id);
    const auto color = colorFor(id);

    const auto spin = time * (0.4f + 0.05f * (float) (id % 17)) + (float) id * 0.61f;
    const auto pulse = 0.72f + 0.28f * std::sin(time * 2.3f + (float) id * 0.37f);
    const auto radius = layout.radius * pulse;

    vertices.clear();
    indices.clear();
    vertices.push_back({{centre.x, centre.y}, {color.r, color.g, color.b}});

    for (auto side = 0; side < sides; ++side)
    {
        const auto angle = spin + (float) side * (2.f * pi / (float) sides);
        vertices.push_back({{centre.x + std::cos(angle) * radius,
                             centre.y + std::sin(angle) * radius},
                            {color.r * 0.34f, color.g * 0.34f, color.b * 0.34f}});
    }

    for (auto side = 0; side < sides; ++side)
    {
        indices.push_back(0);
        indices.push_back((std::uint32_t) (1 + side));
        indices.push_back((std::uint32_t) (1 + (side + 1) % sides));
    }
}

// A different permutation every frame, as a stride coprime with the count: slot
// k draws mesh (k * stride + frame) mod count, which visits every mesh once and
// puts a different one - a different *size* - in every slot than the frame
// before did. Arithmetic rather than std::shuffle because it has to be free at
// two thousand draws a frame, and deterministic so --check repeats.
int strideFor(int count, std::uint64_t frame)
{
    constexpr int candidates[] = {7, 11, 13, 17, 19, 23, 29, 31};
    constexpr auto candidateCount =
        (int) (sizeof(candidates) / sizeof(candidates[0]));
    auto stride = candidates[(int) (frame % (std::uint64_t) candidateCount)];

    while (std::gcd(stride, count) != 1)
        ++stride;

    return stride;
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1100;
    options.height = 720;
    options.minWidth = 480;
    options.minHeight = 360;
    options.title = "eacp GPU - Streaming Stress";
    return options;
}
} // namespace

struct StreamingStressView final : GPUView
{
    StreamingStressView()
        : vertices(BufferUsage::Vertex)
        , indices(BufferUsage::Index)
    {
        vertexScratch.reserve(maxVertices);
        indexScratch.reserve(maxIndices);

        shader.prepare(sampleCount(), false);
        setContinuous(true);
    }

    void update(Threads::FrameTime time) override
    {
        elapsed += (float) time.delta;

        // One second in every three. Read here rather than in render() so that
        // --check, which has no display link, can set it directly.
        surging = std::fmod(elapsed, surgePeriod) >= surgePeriod - surgeLength;
    }

    int meshCount() const { return surging ? baseCount * 2 : baseCount; }

    // Every mesh written, bound and drawn on its own. What they share is the
    // pipeline and the uniform block, which bind() puts in place for the first
    // draw and the rest inherit - re-binding a pipeline two thousand times
    // would be its own benchmark.
    std::size_t drawMeshes(RenderPass& pass, const Layout& layout, int count)
    {
        const auto stride = (std::int64_t) strideFor(count, frameCounter);
        const auto rotation = (std::int64_t) (frameCounter % (std::uint64_t) count);

        auto streamed = std::size_t {0};

        for (auto slot = 0; slot < count; ++slot)
        {
            const auto id =
                (int) (((std::int64_t) slot * stride + rotation) % count);

            buildMesh(vertexScratch, indexScratch, layout, id, elapsed);

            const auto vertexBytes = vertexScratch.size() * sizeof(MeshVertex);
            const auto indexBytes = indexScratch.size() * sizeof(std::uint32_t);

            const auto vertexRange =
                vertices.write(vertexScratch.data(), vertexBytes);
            const auto indexRange = indices.write(indexScratch.data(), indexBytes);

            if (slot == 0)
                pass.bind(shader, vertexRange);
            else
                pass.setVertexBuffer(vertexRange);

            pass.drawIndexed(indexRange, (int) indexScratch.size());

            streamed += vertexBytes + indexBytes;
        }

        return streamed;
    }

    void render(Frame& frame) override
    {
        const auto bounds = getLocalBounds();

        if (bounds.w <= 0.f || bounds.h <= 0.f)
            return;

        const auto started = std::chrono::steady_clock::now();

        const auto count = meshCount();
        const auto aspect = bounds.w / bounds.h;
        const auto layout = layoutFor(count, aspect);

        shader.viewScale = Array {1.f / aspect, 1.f};

        auto descriptor = RenderPassDescriptor {};
        descriptor.clearColor = backgroundColor;

        // A label is what turns the GPU timer on for a pass, so the `gpu`
        // column has something to report.
        descriptor.label = "meshes";

        auto streamed = std::size_t {0};

        {
            auto pass = frame.beginPass(descriptor);
            streamed = drawMeshes(pass, layout, count);
        }

        // After the pass has been destroyed, so the encoder closing is inside
        // the measurement - on a per-draw renderer that is not a rounding error.
        const auto finished = std::chrono::steady_clock::now();

        lastDraws = count;
        lastStreamed = streamed;
        lastLayout = layout;

        cpuSeconds += std::chrono::duration<double>(finished - started).count();
        ++framesSinceReport;
        ++frameCounter;

        report();
    }

    void report()
    {
        if (!printStats || elapsed - lastReportTime < 1.f)
            return;

        const auto created = Device::shared().buffersCreated();
        const auto arenas = vertices.bufferCount() + indices.bufferCount();
        const auto reserved = vertices.bytesReserved() + indices.bytesReserved();

        std::printf("draws %5d   streamed %8.1f KB   created %3d   arenas %2d   "
                    "reserved %8.1f KB   cpu %6.2f ms   gpu %6.2f ms%s\n",
                    lastDraws,
                    (double) lastStreamed / 1024.0,
                    created - buffersAtLastReport,
                    arenas,
                    (double) reserved / 1024.0,
                    1000.0 * cpuSeconds / (double) std::max(1, framesSinceReport),
                    Device::shared().lastFrameTimings().milliseconds,
                    surging ? "   <- surge" : "");

        // A pipe would hold the line, and a scripted run is killed from outside.
        std::fflush(stdout);

        buffersAtLastReport = created;
        cpuSeconds = 0.0;
        framesSinceReport = 0;
        lastReportTime = elapsed;
    }

    MeshShader shader;

    // The two streams a per-draw renderer has, and the reason bufferCount() is
    // six rather than three at rest: one arena per frame in flight, per usage.
    StreamingBuffers vertices;
    StreamingBuffers indices;

    std::vector<MeshVertex> vertexScratch;
    std::vector<std::uint32_t> indexScratch;

    int baseCount = defaultMeshCount;

    // Driven by update() in the window and set by hand in --check, which has no
    // display link to drive anything.
    bool surging = false;
    float elapsed = 0.f;

    // Bumped by render() rather than taken from Device::frameIndex(), so
    // --check sees the same fourteen permutations however many frames the
    // process drew before it.
    std::uint64_t frameCounter = 0;

    bool printStats = false;

    int lastDraws = 0;
    std::size_t lastStreamed = 0;
    Layout lastLayout {};

    double cpuSeconds = 0.0;
    int framesSinceReport = 0;
    int buffersAtLastReport = 0;
    float lastReportTime = 0.f;
};

struct StreamingStressApp
{
    StreamingStressApp()
    {
        view.printStats = true;
        window.setContentView(view);

        std::printf("%d meshes a frame, each its own write + bind + drawIndexed; "
                    "the load doubles for one second in every three.\n"
                    "`created` is the number that matters: GPU buffers made in "
                    "the last second, which is zero once the arenas are warm.\n\n",
                    defaultMeshCount);
        std::fflush(stdout);
    }

    StreamingStressView view;
    Graphics::Window window {windowOptions()};
};

namespace
{
constexpr auto checkSize = 512;
constexpr auto checkFrames = 14;

// One whole period at double the load, so every pool grows and the three frames
// after it are the three folds. In the middle, so there are steady frames on
// both sides of it.
constexpr auto surgeFirstFrame = 6;
constexpr auto surgeLastFrame = 8;

struct FrameRecord
{
    int draws = 0;
    double kilobytes = 0.0;
    int created = 0;
    int arenas = 0;
};

// Frames whose allocation count has to be zero: past the warm-up, and clear of
// the surge and of the folds it causes one period later.
bool isSteadyFrame(int frame)
{
    if (frame < 2 * StreamingBuffers::framesInFlight)
        return false;

    return frame < surgeFirstFrame || frame > surgeLastFrame + 2;
}

float luminanceOf(const Graphics::Color& color)
{
    return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
}

// The brightest of the nine pixels around a cell's centre. A mesh's bright
// vertex is its centre one and sits exactly there, and the maximum over a small
// block keeps a one-pixel rounding difference from failing a real picture.
float cellBrightness(const Graphics::Image& image, const Layout& layout, int id)
{
    const auto column = id % layout.columns;
    const auto row = id / layout.columns;

    const auto centreX =
        (float) image.width() * ((float) column + 0.5f) / (float) layout.columns;

    // Row 0 of the grid is at the bottom in clip space and at the bottom of the
    // image, which is the last row of pixels.
    const auto centreY =
        (float) image.height() * (1.f - ((float) row + 0.5f) / (float) layout.rows);

    auto brightest = 0.f;

    for (auto dy = -1; dy <= 1; ++dy)
        for (auto dx = -1; dx <= 1; ++dx)
            brightest = std::max(
                brightest,
                luminanceOf(image.at((int) centreX + dx, (int) centreY + dy)));

    return brightest;
}

// Fourteen frames with no window: the same writes, the same shuffle and the
// same surge, with the buffer count read either side of every one of them.
//
// Each renderToImage is one device frame - Frame's constructor advances
// Device::frameIndex() whether or not anything is on screen - so the pools
// rotate across the calls as they would in a window, which is what makes
// fourteen of these a test of the recycling rather than of one pool.
int runCheck()
{
    auto view = StreamingStressView {};
    view.setBounds({0.f, 0.f, (float) checkSize, (float) checkSize});

    auto records = std::vector<FrameRecord> {};
    auto image = Graphics::Image {};

    records.reserve(checkFrames);

    for (auto frame = 0; frame < checkFrames; ++frame)
    {
        view.surging = frame >= surgeFirstFrame && frame <= surgeLastFrame;

        // Nothing drives update() here, so time is advanced by hand - the
        // meshes still spin and pulse across the fourteen frames, which is what
        // keeps their byte counts from being identical frame to frame.
        view.elapsed = (float) frame / 60.f;

        const auto before = Device::shared().buffersCreated();

        image = view.renderToImage(1.f);

        if (!image.isValid())
        {
            std::printf("no GPU device\n");
            return 1;
        }

        records.push_back(
            {view.lastDraws,
             (double) view.lastStreamed / 1024.0,
             Device::shared().buffersCreated() - before,
             view.vertices.bufferCount() + view.indices.bufferCount()});
    }

    std::printf(
        "%d x %d, %d frames, %d meshes a frame (doubled over frames %d-%d)\n\n",
        checkSize,
        checkSize,
        checkFrames,
        defaultMeshCount,
        surgeFirstFrame,
        surgeLastFrame);

    std::printf("frame   draws   streamed KB   buffers created   arenas alive\n");

    for (auto frame = 0; frame < (int) records.size(); ++frame)
    {
        const auto& record = records[(std::size_t) frame];

        std::printf("%5d   %5d   %11.1f   %15d   %12d%s\n",
                    frame,
                    record.draws,
                    record.kilobytes,
                    record.created,
                    record.arenas,
                    isSteadyFrame(frame) ? "   steady" : "");
    }

    auto failures = 0;

    std::printf("\nnothing allocated on a steady frame\n");
    for (auto frame = 0; frame < (int) records.size(); ++frame)
    {
        if (!isSteadyFrame(frame) || records[(std::size_t) frame].created == 0)
            continue;

        std::printf("  frame %d created %d buffers\n",
                    frame,
                    records[(std::size_t) frame].created);
        ++failures;
    }

    if (failures == 0)
        std::printf("  every steady frame created 0\n");

    const auto arenas = view.vertices.bufferCount() + view.indices.bufferCount();
    const auto expected = 2 * StreamingBuffers::framesInFlight;

    if (arenas != expected)
        ++failures;

    std::printf("\narenas folded back to one per pool\n");
    std::printf("  %d alive, %d expected (%d frames in flight x 2 streams)\n",
                arenas,
                expected,
                StreamingBuffers::framesInFlight);
    std::printf(
        "  %.1f KB reserved across them\n",
        (double) (view.vertices.bytesReserved() + view.indices.bytesReserved())
            / 1024.0);

    // Nine cells spread over the grid, which is what says the slices were read
    // at their own offsets: a bind ignoring the offset would draw the arena's
    // first mesh two thousand times and leave the rest at the clear colour.
    const auto& layout = view.lastLayout;

    std::printf("\nthe last frame drew meshes where they belong\n");

    for (auto row = 1; row <= 3; ++row)
        for (auto column = 1; column <= 3; ++column)
        {
            const auto id = (layout.rows * row / 4) * layout.columns
                            + layout.columns * column / 4;

            if (id >= view.lastDraws)
                continue;

            const auto brightness = cellBrightness(image, layout, id);
            std::printf("  cell %5d  brightness %.3f%s\n",
                        id,
                        (double) brightness,
                        brightness > 0.15f ? "" : "   <- background");

            if (brightness <= 0.15f)
                ++failures;
        }

    if (failures > 0)
    {
        std::printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }

    std::printf("\nok\n");
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--check") == 0)
        return runCheck();

    return eacp::Apps::run<StreamingStressApp>();
}
