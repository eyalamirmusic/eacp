#include "Common.h"

#include <cmath>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;

// 0.25, 0.5, 0.75 - ascending, so a pixel in the wrong order says the buffer
// was read at the wrong stride rather than that the values were wrong.
constexpr auto channelBase = 0.25f;
constexpr auto channelStep = 0.25f;
constexpr auto channelCount = 3;

struct QuadVertex
{
    float position[2];
};

// The bytes the kernel indexes as a flat float array, read at this stride.
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

// Deliberately a function of the thread id, so a kernel that never ran leaves
// the buffer's zeros behind and the difference is visible in the pixel.
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

// The clear colour is deliberately nothing like the computed one, so a draw
// that did not happen cannot pass by accident.
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

// Red rises across the width and blue falls with it, so a texel from the wrong
// column is a different colour; green rises down the height, so y is checked
// too, whichever way up the sampled image ends on screen.
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

// Sampled a texel at a time, so what comes back is not a blend.
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

// The texture is never touched by the CPU and the clear is black, so every
// colour on screen came through the kernel.
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

// Deliberately not record 0: a shader that dropped the index would read the
// first record and still produce a plausible colour.
constexpr auto paletteRecords = 2;
constexpr auto paletteRecord = 1;
constexpr auto paletteBase = 0.1f;
constexpr auto paletteStep = 0.15f;
constexpr auto paletteFloats = paletteRecords * channelCount;

constexpr float paletteValueAt(int element)
{
    return paletteBase + (float) element * paletteStep;
}

// Picks its colour out of a storage buffer at an index it computes, rather
// than receiving it as a per-instance attribute.
struct ColorFromIndexedBuffer final : ShaderProgram
{
    ColorFromIndexedBuffer() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(palette.read3(record), 1.f));
    }

    Uniform<InputBuffer> palette;
    Uniform<UInt> record;

    EACP_SHADER(palette, record)
};

// No per-instance stream carries the colour: the buffer is bound whole and the
// shader indexes it.
struct ComputeThenIndexedDrawView final : GPUView
{
    ComputeThenIndexedDrawView()
        : palette(Device::shared(),
                  nullptr,
                  sizeof(float) * paletteFloats,
                  BufferUsage::Storage)
    {
        setSampleCount(1);

        kernel.output = palette;
        kernel.base = paletteBase;
        kernel.stepSize = paletteStep;
        kernel.prepare();

        draw.setVertices(fullQuad, 6);
        draw.palette = palette;
        draw.record = (std::uint32_t) paletteRecord;
        draw.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        {
            auto compute = frame.beginCompute();
            compute.dispatch(kernel, paletteFloats);
        }

        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.draw(draw);
    }

    Buffer palette;
    ColorKernel kernel;
    ColorFromIndexedBuffer draw;
};

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

// The components come back rotated, so a stride the read and the write
// disagreed on is a wrong number in the wrong place, not the input handed back.
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

// Each cell writes a value saying which cell it was, so a wrong stride - or a
// width and height the wrong way round - is a wrong number, not a missing one.
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

// The buffer starts uninitialised, the CPU never writes it and the clear is
// black, so no other path could have put the colour there.
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

    // The tolerance is what the read-back's transfer through the compositor
    // leaves.
    check(std::abs(pixel.r - channelBase) < 0.1f);
    check(std::abs(pixel.b - (channelBase + 2.f * channelStep)) < 0.1f);
};

// The texture is created empty, the CPU never writes it and the clear is black.
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

    // Running the way the kernel painted it, which says x reached the write.
    check(right.r > left.r + 0.3f);
    check(left.b > right.b + 0.3f);

    // Screen column 1 samples texel 1 and column width-2 samples texel width-2.
    constexpr auto lastColumn = (float) (viewWidth - 1);
    check(std::abs(left.r - 1.f / lastColumn) < 0.15f);
    check(std::abs(right.r - (lastColumn - 1.f) / lastColumn) < 0.15f);

    // Whichever way up the sampled image lands, two rows apart must differ,
    // which they cannot if y never reached the write.
    auto top = image.at(viewWidth / 2, 0);
    auto bottom = image.at(viewWidth / 2, viewHeight - 1);
    check(std::abs(top.g - bottom.g) > 0.3f);
};

// Reading the wrong record, or dropping the index, gives the other record's
// colour - a different pixel, not a missing one.
auto tIndexedBufferReadFeedsTheDraw =
    test("FrameCompute/indexedBufferReadFeedsTheDraw") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ComputeThenIndexedDrawView {};
    auto image = readBack(view);

    check(image.isValid());

    auto pixel = image.at(viewWidth / 2, viewHeight / 2);

    // The record the shader was pointed at, not the one before it.
    constexpr auto first = paletteRecord * channelCount;

    check(std::abs(pixel.r - paletteValueAt(first)) < 0.1f);
    check(std::abs(pixel.g - paletteValueAt(first + 1)) < 0.1f);
    check(std::abs(pixel.b - paletteValueAt(first + 2)) < 0.1f);

    // And distinguishable from record 0, so the checks above could have failed.
    check(std::abs(paletteValueAt(first) - paletteValueAt(0)) > 0.2f);
};

// The index is in records on both sides, so the kernel never spells the stride.
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

// BGRA8Unorm is worth naming: it is the drawable's own format, so it is what
// someone reaches for first.
auto tComputeWriteRefusesUnsupportedFormat =
    test("FrameCompute/computeWriteRefusesBGRA") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = computeTarget();
    descriptor.format = TextureFormat::BGRA8Unorm;

    auto texture = Device::shared().makeTexture(descriptor);

    check(!texture.isValid());
    check(!texture.isComputeWritable());
};

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

// Each cell's value is a function of both coordinates, so an off-by-one grid,
// swapped extents and a guard that never fired all read differently.
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

// A read() straight after commitAsync is only correct if it orders behind the
// submission - the guarantee the Metal path grew once commit stopped blocking.
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

namespace
{
// A tree fold, one halving per barrier. Out-of-range threads load zero rather
// than returning early: a kernel that barriers has no early return to take.
struct GroupSumKernel final : ComputeProgram
{
    GroupSumKernel() { compile(); }

    void define() override
    {
        auto gid = threadId();
        auto lid = localId();
        auto group = groupId();
        auto tile = shared<Float>(groupWidth);

        auto value = var(0.0f);
        ifThen(gid < gridCount(), [&] { value = input[gid]; });
        write(tile, lid, value.get());
        barrier();

        for (auto stride = groupWidth / 2; stride > 0; stride /= 2)
        {
            auto bound = (unsigned) stride;

            ifThen(lid < bound,
                   [&] { write(tile, lid, tile[lid] + tile[lid + bound]); });
            barrier();
        }

        ifThen(lid == 0u, [&] { write(output, group, tile[0u]); });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};
} // namespace

// The grid is deliberately not a multiple of the group, so the excess threads
// run the whole body and must contribute zeros rather than garbage or a hang.
// Small integers sum exactly in float, so the checks are equalities.
auto tSharedMemoryGroupSums = test("FrameCompute/sharedMemoryGroupSums") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 130;
    constexpr auto width = ComputePass::threadGroupWidth;
    constexpr auto groups = (count + width - 1) / width;

    float values[count] = {};

    for (auto i = 0; i < count; ++i)
        values[i] = (float) (i % 7 + 1);

    auto source = device.makeBuffer(values, sizeof(values), BufferUsage::Storage);
    auto sums = device.makeBuffer(sizeof(float) * groups, BufferUsage::Storage);

    auto kernel = GroupSumKernel {};
    kernel.input = source;
    kernel.output = sums;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, count);
        }

        commands.commit();
    }

    float result[groups] = {};
    sums.read(result, sizeof(result));

    for (auto group = 0; group < groups; ++group)
    {
        auto last = group == groups - 1 ? count : (group + 1) * width;
        auto expected = 0.0f;

        for (auto i = group * width; i < last; ++i)
            expected += values[i];

        check(result[group] == expected);
    }
};
