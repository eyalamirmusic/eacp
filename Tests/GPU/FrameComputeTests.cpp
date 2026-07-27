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

template <typename View>
Graphics::Image readBack(View& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
    return view.renderToImage(1.f);
}

// The image a kernel paints, one texel per work item. Red rises across the
// width and blue falls with it, so a texel that came from the wrong column is
// a different colour rather than a missing one; green rises down the height, so
// the second coordinate is checked too - whichever way up the sampled image
// ends on screen.
struct ImageKernel final : ComputeProgram
{
    ImageKernel() { compile(); }

    void define() override
    {
        auto p = threadPosition();
        auto across = toFloat(p.x) * (1.f / (float) (viewWidth - 1));
        auto down = toFloat(p.y) * (1.f / (float) (viewHeight - 1));

        write(target, p.x, p.y, float4(across, down, 1.f - across, 1.f));
    }

    Uniform<WritableTexture2D> target;

    EACP_SHADER(target)
};

// Fills the drawable with that image, sampled a texel at a time so what comes
// back is what the kernel wrote rather than a blend of it.
struct DrawImage final : ShaderProgram
{
    DrawImage()
    {
        image.sampling = {TextureFilter::Nearest, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = varying(position * 0.5f + 0.5f);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(sample(image, uv));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

TextureDescriptor computeTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = viewWidth;
    descriptor.height = viewHeight;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.computeWrite = true;
    return descriptor;
}

// A kernel's image drawn by the next pass on the same frame - the whole point
// of a texture a kernel can write. The texture is never touched by the CPU and
// the clear is black, so every colour on screen came through the kernel.
struct ComputeImageThenDrawView final : GPUView
{
    ComputeImageThenDrawView()
        : target(Device::shared().makeTexture(computeTarget()))
    {
        setSampleCount(1);

        kernel.target = target;
        kernel.prepare();

        draw.setVertices(fullQuad, 6);
        draw.image = target;
        draw.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        {
            auto compute = frame.beginCompute();
            compute.dispatch(kernel, viewWidth, viewHeight);
        }

        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.draw(draw);
    }

    Texture target;
    ImageKernel kernel;
    DrawImage draw;
};

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

// A kernel over records of four floats rather than single ones. The components
// come back rotated, so a stride the read and the write disagreed on shows up
// as the wrong number in the wrong place instead of as the input handed back.
struct RotateRecords final : ComputeProgram
{
    RotateRecords() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto record = input.read4(i);

        write(output, i, float4(record.w(), record.x(), record.y(), record.z()));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

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

// The compute-to-fragment path: a kernel writes a texture and the next pass on
// the same frame samples it. Nothing here could have come from anywhere else -
// the texture is created empty, the CPU never writes it, and the clear is
// black.
auto tKernelImageFeedsTheDraw = test("FrameCompute/kernelImageFeedsTheDraw") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ComputeImageThenDrawView {};
    check(view.target.isComputeWritable());

    auto image = readBack(view);
    check(image.isValid());

    auto left = image.at(1, viewHeight / 2);
    auto right = image.at(viewWidth - 2, viewHeight / 2);

    // Not the clear, and running the way the kernel painted it across the
    // width - which is also what says the x coordinate reached the write.
    check(right.r > left.r + 0.3f);
    check(left.b > right.b + 0.3f);

    // And close to the ramp itself: screen column 1 samples texel 1 and column
    // width-2 samples texel width-2, within what the read-back's transfer
    // through the compositor leaves.
    constexpr auto lastColumn = (float) (viewWidth - 1);
    check(std::abs(left.r - 1.f / lastColumn) < 0.15f);
    check(std::abs(right.r - (lastColumn - 1.f) / lastColumn) < 0.15f);

    // And down the height. Whichever way up the sampled image lands on screen,
    // two rows apart must differ, which they cannot if the y coordinate never
    // reached the write.
    auto top = image.at(viewWidth / 2, 0);
    auto bottom = image.at(viewWidth / 2, viewHeight - 1);
    check(std::abs(top.g - bottom.g) > 0.3f);
};

// read4 and the Float4 write address the same record: the index is in records
// on both sides, so the kernel never spells the stride and the bytes that come
// back are the ones the rotation predicts.
auto tVectorElementsRoundTrip = test("FrameCompute/vectorElementsRoundTrip") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto floatsPerRecord = 4;
    constexpr auto records = kernelCount / floatsPerRecord;

    auto source = device.makeBuffer(kernelInput, BufferUsage::Storage);
    auto output = device.makeBuffer(sizeof(kernelInput), BufferUsage::Storage);

    auto kernel = RotateRecords {};
    kernel.input = source;
    kernel.output = output;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, records);
        }

        commands.commit();
    }

    float result[kernelCount] = {};
    output.read(result, sizeof(result));

    for (auto record = 0; record < records; ++record)
    {
        const auto* in = kernelInput + record * floatsPerRecord;
        const auto* out = result + record * floatsPerRecord;

        check(out[0] == in[3]);
        check(out[1] == in[0]);
        check(out[2] == in[1]);
        check(out[3] == in[2]);
    }
};

// A format outside the guaranteed set is refused at creation rather than
// binding as an output that silently writes nothing. BGRA8Unorm is the one
// worth naming: it is the drawable's own format, so it is exactly what someone
// reaches for first.
auto tComputeWriteRefusesUnsupportedFormat =
    test("FrameCompute/computeWriteRefusesBGRA") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = computeTarget();
    descriptor.format = TextureFormat::BGRA8Unorm;

    auto texture = Device::shared().makeTexture(descriptor);

    // Nothing was created, so this is as loud as it gets short of throwing:
    // the texture does not work at all rather than working everywhere except
    // in the kernel that was the reason for asking.
    check(!texture.isValid());
    check(!texture.isComputeWritable());
};

// And a plain texture is not one either, so setOutputTexture has something to
// refuse rather than binding a resource with no view to bind through.
auto tPlainTextureIsNotComputeWritable =
    test("FrameCompute/plainTextureIsNotComputeWritable") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = computeTarget();
    descriptor.computeWrite = false;

    auto texture = Device::shared().makeTexture(descriptor);
    check(texture.isValid());
    check(!texture.isComputeWritable());
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
