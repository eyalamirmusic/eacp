#include "Common.h"

#include <cmath>

// Compute that never reaches the CPU, and a commit that does not wait.
//
// Frame::beginCompute puts a kernel on the frame's own command buffer, so what
// it writes is legal to draw in the very next pass. There is no CPU-side
// observable for that - the buffer is written and consumed on the GPU - so the
// only way to check it is to make the kernel's output decide a pixel and read
// the pixel back.
//
// CommandBuffer::commitAsync is the other half: the submission returns before
// the GPU has run, and the Async says when it has. What has to hold is that
// nothing about correctness changed - the same kernel gives the same answer,
// and a read() that does not wait for the Async still sees finished data.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;

// The colour the kernel computes, one channel per element: 0.25, 0.5, 0.75.
// Ascending, so a pixel that came back in the wrong order says the buffer was
// read at the wrong stride rather than that the values were wrong.
constexpr auto channelBase = 0.25f;
constexpr auto channelStep = 0.25f;
constexpr auto channelCount = 3;

struct QuadVertex
{
    float position[2];
};

// One instance, whose colour is the three floats the kernel wrote. The same
// bytes the kernel indexes as a flat float array are read here at this struct's
// stride, which is the whole compute-feeds-graphics contract in one type.
struct InstanceColor
{
    float color[3];
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

// Writes channelBase + index * channelStep. Deliberately a function of the
// thread id, so a kernel that never ran leaves the buffer's zeros behind and
// the difference is visible in the pixel.
struct ColorKernel final : ComputeProgram
{
    ColorKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, base + toFloat(i) * stepSize);
    }

    Uniform<OutputBuffer> output;
    Uniform<Float> base;
    Uniform<Float> stepSize;

    EACP_SHADER(output, base, stepSize)
};

// Fills the drawable with the instance colour, which came from the buffer the
// kernel wrote.
struct ColorFromBuffer final : ShaderProgram
{
    ColorFromBuffer() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto color = instanceInput(&InstanceColor::color, 1);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(varying(color), 1.f));
    }

    EACP_SHADER()
};

// One frame: a compute pass writes the colour, the render pass draws it. The
// clear colour is deliberately nothing like the computed one, so a draw that
// somehow did not happen cannot pass by accident.
struct ComputeThenDrawView final : GPUView
{
    ComputeThenDrawView()
        : colors(Device::shared(),
                 nullptr,
                 sizeof(float) * channelCount,
                 BufferUsage::Storage)
    {
        setSampleCount(1);

        kernel.output = colors;
        kernel.base = channelBase;
        kernel.stepSize = channelStep;
        kernel.prepare();

        draw.setVertices(fullQuad, 6);
        draw.setInstanceBuffer(1, colors, 1);
        draw.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        {
            auto compute = frame.beginCompute();
            compute.dispatch(kernel, channelCount);
        }

        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.drawInstanced(draw, 1);
    }

    Buffer colors;
    ColorKernel kernel;
    ColorFromBuffer draw;
};

Graphics::Image readBack(ComputeThenDrawView& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
    return view.renderToImage(1.f);
}

// The kernel both commit paths run, kept apart from the frame test's so each
// says one thing.
struct ScaleKernel final : ComputeProgram
{
    ScaleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * scale);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> scale;

    EACP_SHADER(input, output, scale)
};

constexpr float kernelInput[] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
constexpr auto kernelCount = (int) (sizeof(kernelInput) / sizeof(kernelInput[0]));
constexpr auto kernelScale = 3.f;

// A kernel over a grid rather than a flat count. Each cell writes a value that
// says which cell it was, so a result read back at the wrong stride - or a
// width and a height packed the wrong way round - shows up as a wrong number
// rather than as a missing one.
struct GridKernel final : ComputeProgram
{
    GridKernel() { compile(); }

    void define() override
    {
        auto p = threadPosition();
        write(output, p.y * stride + p.x, toFloat(p.x) + toFloat(p.y) * 100.f);
    }

    Uniform<OutputBuffer> output;
    Uniform<UInt> stride;

    EACP_SHADER(output, stride)
};

// Deliberately not multiples of the 8x8 threadgroup: what the guard drops is
// what would otherwise write past the end of a row.
constexpr auto gridWidth = 5;
constexpr auto gridHeight = 3;
} // namespace

// A kernel's output drawn by the next pass on the same frame. The pixel is the
// colour the kernel computed, which no other path could have put there: the
// buffer starts uninitialised, the CPU never writes it, and the clear is black.
auto tKernelOutputFeedsDraw = test("FrameCompute/kernelOutputFeedsTheDraw") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ComputeThenDrawView {};
    auto image = readBack(view);

    check(image.isValid());

    auto pixel = image.at(viewWidth / 2, viewHeight / 2);

    // Not the clear, and in the ascending order the kernel wrote.
    check(pixel.r > 0.1f);
    check(pixel.g > pixel.r + 0.1f);
    check(pixel.b > pixel.g + 0.1f);

    // And close to the values themselves, within what the read-back's transfer
    // through the compositor leaves.
    check(std::abs(pixel.r - channelBase) < 0.1f);
    check(std::abs(pixel.b - (channelBase + 2.f * channelStep)) < 0.1f);
};

// commitAsync submits without waiting and resolves once the GPU is done, with
// the same output the blocking commit produces.
auto tCommitAsyncMatchesCommit = test("FrameCompute/commitAsyncMatchesCommit") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto source = device.makeBuffer(kernelInput, BufferUsage::Storage);
    auto blocking = device.makeBuffer(sizeof(kernelInput), BufferUsage::Storage);
    auto async = device.makeBuffer(sizeof(kernelInput), BufferUsage::Storage);

    auto kernel = ScaleKernel {};
    kernel.input = source;
    kernel.scale = kernelScale;
    kernel.prepare();

    {
        kernel.output = blocking;
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, kernelCount);
        }

        commands.commit();
    }

    auto finished = Threads::Async<void> {};

    {
        kernel.output = async;
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, kernelCount);
        }

        finished = commands.commitAsync();
    }

    // Resolving needs the event loop to turn, which is what waitFor pumps.
    finished.waitFor(Time::MS {5000});
    check(finished.isResolved());

    float fromBlocking[kernelCount] = {};
    float fromAsync[kernelCount] = {};
    blocking.read(fromBlocking, sizeof(fromBlocking));
    async.read(fromAsync, sizeof(fromAsync));

    for (auto i = 0; i < kernelCount; ++i)
    {
        check(fromBlocking[i] == kernelInput[i] * kernelScale);
        check(fromAsync[i] == fromBlocking[i]);
    }
};

// A 2D dispatch: every cell of the grid runs exactly once, and no thread
// outside it writes anything. The buffer starts at zero and the value each cell
// writes is a function of both its coordinates, so an off-by-one grid, a
// swapped pair of extents and a guard that never fired are all distinguishable
// from the numbers that come back.
auto tGridDispatchCoversTheGrid = test("FrameCompute/gridDispatchCoversTheGrid") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto cells = gridWidth * gridHeight;
    auto output = device.makeBuffer(sizeof(float) * cells, BufferUsage::Storage);

    auto kernel = GridKernel {};
    kernel.output = output;
    kernel.stride = (std::uint32_t) gridWidth;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, gridWidth, gridHeight);
        }

        commands.commit();
    }

    float result[cells] = {};
    output.read(result, sizeof(result));

    for (auto y = 0; y < gridHeight; ++y)
        for (auto x = 0; x < gridWidth; ++x)
            check(result[y * gridWidth + x] == (float) x + (float) y * 100.f);
};

// The read that does not wait. commitAsync returns before the GPU has run, so a
// read() straight afterwards is only correct if the read itself orders behind
// the submission - which is the guarantee the Metal path had to grow once the
// commit stopped blocking, and the one D3D12 already had from its fence.
auto tReadAfterCommitAsyncIsOrdered =
    test("FrameCompute/readAfterCommitAsyncSeesTheResult") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto source = device.makeBuffer(kernelInput, BufferUsage::Storage);
    auto output = device.makeBuffer(sizeof(kernelInput), BufferUsage::Storage);

    auto kernel = ScaleKernel {};
    kernel.input = source;
    kernel.output = output;
    kernel.scale = kernelScale;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, kernelCount);
        }

        // Deliberately dropped: no waitFor, no then(), nothing pumping the
        // loop between the submission and the read below.
        commands.commitAsync();
    }

    float result[kernelCount] = {};
    output.read(result, sizeof(result));

    for (auto i = 0; i < kernelCount; ++i)
        check(result[i] == kernelInput[i] * kernelScale);
};
