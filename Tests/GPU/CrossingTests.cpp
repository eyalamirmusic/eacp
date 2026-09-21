#include "Common.h"

#include <cmath>

// What it costs to have two backends behind one Device, and that the pictures
// come out the same anyway (plan.md D11).
//
// Every case here is written about the API and not about the composite: a
// kernel writes something, a draw reads it, and the pixel or the byte that
// comes back is the same on Metal, D3D12, Vulkan and OpenGL-with-Vulkan alike.
// What the composite adds is the price, and Device::crossingBytesThisFrame()
// is where it shows - zero on every device that has one backend, and exactly
// the resource, or exactly the rectangle of it that was written, on the one
// that has two. So the same case says "this works" everywhere and "and this is
// what it cost" on the device where it costs anything.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;

constexpr auto imageWidth = 16;
constexpr auto imageHeight = 16;

constexpr auto imageBytes = imageWidth * imageHeight * 4;

// The rectangle of that image a host update writes, small enough that the
// rectangle crossing and the whole-resource one cannot be confused.
constexpr auto regionSize = 4;
constexpr auto regionBytes = regionSize * regionSize * 4;

struct QuadVertex
{
    float position[2];
};

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

TextureDescriptor computeTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = imageWidth;
    descriptor.height = imageHeight;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.computeWrite = true;
    return descriptor;
}

TextureDescriptor renderTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = imageWidth;
    descriptor.height = imageHeight;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    return descriptor;
}

// Paints a flat colour into a texture a kernel can write.
struct FlatImageKernel final : ComputeProgram
{
    FlatImageKernel() { compile(); }

    void define() override
    {
        auto p = threadPosition();

        write(target, p.x, p.y, float4(constant(0.f), 0.75f, 0.25f, 1.f));
    }

    Uniform<WritableTexture2D> target;

    EACP_SHADER(target)
};

// Reads one texel of a texture and puts its green channel in a buffer, which is
// how a rendered picture gets back to the CPU through a kernel rather than
// through a read-back.
struct SampleToBufferKernel final : ComputeProgram
{
    SampleToBufferKernel() { compile(); }

    void define() override
    {
        auto texel = sample(source, float2(constant(0.5f), constant(0.5f)));

        write(output, threadId(), texel.y());
    }

    Uniform<Texture2D> source;
    Uniform<OutputBuffer> output;

    EACP_SHADER(source, output)
};

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

struct FlatColor final : ShaderProgram
{
    FlatColor() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(constant(0.f), 0.75f, 0.25f, 1.f));
    }

    EACP_SHADER()
};

struct ColorFromStorageBuffer final : ShaderProgram
{
    ColorFromStorageBuffer() { compile(); }

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

struct ColorFromInstance final : ShaderProgram
{
    ColorFromInstance() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto color = instanceInput(&InstanceColor::color, 1);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(varying(color), 1.f));
    }

    EACP_SHADER()
};

// A kernel's texture sampled by the pass after it, which is the crossing D11
// was written for: a whole resource, once a frame.
struct KernelImageView final : GPUView
{
    KernelImageView()
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
            compute.dispatch(kernel, imageWidth, imageHeight);
        }

        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});
        pass.draw(draw);
    }

    Texture target;
    FlatImageKernel kernel;
    DrawImage draw;
};

// The same, for a buffer: a kernel writes three floats and the draw takes them
// as a per-instance attribute.
struct KernelBufferView final : GPUView
{
    KernelBufferView()
        : colors(Device::shared(), nullptr, sizeof(float) * 3, BufferUsage::Storage)
    {
        setSampleCount(1);

        kernel.output = colors;
        kernel.base = 0.25f;
        kernel.stepSize = 0.25f;
        kernel.prepare();

        draw.setVertices(fullQuad, 6);
        draw.setInstanceBuffer(1, colors, 1);
        draw.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        {
            auto compute = frame.beginCompute();
            compute.dispatch(kernel, 3);
        }

        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});
        pass.drawInstanced(draw, 1);
    }

    Buffer colors;
    ColorKernel kernel;
    ColorFromInstance draw;
};

// The other direction: a pass paints a texture and the kernel after it reads
// what the pass left there.
struct RenderThenKernelView final : GPUView
{
    RenderThenKernelView()
        : target(Device::shared().makeTexture(renderTarget()))
        , readBack(Device::shared(), nullptr, sizeof(float), BufferUsage::Storage)
    {
        setSampleCount(1);

        paint.setVertices(fullQuad, 6);
        paint.prepare(1,
                      false,
                      PrimitiveTopology::Triangles,
                      BlendMode::None,
                      pixelFormatFor(TextureFormat::RGBA8Unorm));

        kernel.source = target;
        kernel.output = readBack;
        kernel.prepare();
    }

    void render(Frame& frame) override
    {
        {
            auto into = frame.beginPass(target, {{0.f, 0.f, 0.f, 1.f}});
            into.draw(paint);
        }

        auto compute = frame.beginCompute();
        compute.dispatch(kernel, 1);
    }

    Texture target;
    Buffer readBack;
    FlatColor paint;
    SampleToBufferKernel kernel;
};

// A kernel's buffer bound to the fragment stage as a storage buffer rather than
// streamed in as an attribute, which is the bind a GL with no std430 block
// cannot make. Nothing else in the view crosses, so what the counters report is
// this buffer or nothing.
struct KernelStorageBufferView final : GPUView
{
    KernelStorageBufferView()
        : palette(Device::shared(), nullptr, sizeof(float) * 3, BufferUsage::Storage)
    {
        setSampleCount(1);

        kernel.output = palette;
        kernel.base = 0.25f;
        kernel.stepSize = 0.25f;
        kernel.prepare();

        draw.setVertices(fullQuad, 6);
        draw.palette = palette;
        draw.record = 0u;
        draw.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        {
            auto compute = frame.beginCompute();
            compute.dispatch(kernel, 3);
        }

        auto pass = frame.beginPass({{1.f, 0.f, 0.f, 1.f}});
        pass.draw(draw);
    }

    Buffer palette;
    ColorKernel kernel;
    ColorFromStorageBuffer draw;
};

// A texture a kernel only reads, so that what crosses is what the host wrote
// into it and nothing else.
struct HostWriteThenKernelView final : GPUView
{
    HostWriteThenKernelView()
        : source(Device::shared().makeTexture(computeTarget()))
        , readBack(Device::shared(), nullptr, sizeof(float), BufferUsage::Storage)
    {
        setSampleCount(1);

        kernel.source = source;
        kernel.output = readBack;
        kernel.prepare();
    }

    void render(Frame& frame) override
    {
        auto compute = frame.beginCompute();
        compute.dispatch(kernel, 1);
    }

    Texture source;
    Buffer readBack;
    SampleToBufferKernel kernel;
};

template <typename View>
Graphics::Image renderOnce(View& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
    return view.renderToImage(1.f);
}

bool isKernelGreen(const Graphics::Color& pixel)
{
    return std::abs(pixel.r - 0.f) < 0.15f && std::abs(pixel.g - 0.75f) < 0.15f
           && std::abs(pixel.b - 0.25f) < 0.15f;
}
} // namespace

// A texture a kernel wrote, sampled by the very next pass. On a composite that
// texture lives on both halves and the whole of it is copied between them,
// because a dispatch reports no rectangle; everywhere else it is one resource
// and nothing moves.
auto tKernelTextureReachesTheDraw =
    test("Crossing/aKernelsTextureIsSampledByTheDraw") = []
{
    if (!computeIsAvailable())
        return;

    auto& device = Device::shared();
    auto view = KernelImageView {};

    if (!view.target.isValid())
        return;

    auto image = renderOnce(view);

    check(image.isValid());
    check(isKernelGreen(image.at(viewWidth / 2, viewHeight / 2)),
          "the draw shows what the kernel wrote, not the clear");

    if (!deviceCrossesResources())
    {
        check(device.crossingBytesThisFrame() == 0,
              "one backend has nothing to cross");
        check(device.crossingsThisFrame() == 0);
        return;
    }

    check(device.crossingsThisFrame() >= 1, "the texture crossed");
    check(device.crossingBytesThisFrame() >= imageBytes,
          "and a dispatch names no rectangle, so the whole of it did");
};

// The same for a buffer, whose twin is made the first time a kernel binds it
// and read back into the render side's before the draw takes it as a stream.
auto tKernelBufferReachesTheDraw =
    test("Crossing/aKernelsBufferIsDrawnAsVertices") = []
{
    if (!computeIsAvailable())
        return;

    auto& device = Device::shared();
    auto view = KernelBufferView {};

    if (!view.colors.isValid())
        return;

    auto image = renderOnce(view);

    check(image.isValid());

    auto pixel = image.at(viewWidth / 2, viewHeight / 2);

    check(std::abs(pixel.r - 0.25f) < 0.1f);
    check(std::abs(pixel.g - 0.5f) < 0.1f);
    check(std::abs(pixel.b - 0.75f) < 0.1f);

    if (!deviceCrossesResources())
    {
        check(device.crossingBytesThisFrame() == 0);
        return;
    }

    // One crossing and no more: the buffer was created empty, so the twin owes
    // the render side nothing when it is made and only what the kernel wrote
    // comes back.
    check(device.crossingsThisFrame() == 1);
    check(device.crossingBytesThisFrame() == (std::int64_t) (sizeof(float) * 3));
};

// The reverse crossing: a pass paints a texture and the kernel after it reads
// what is there. The value comes back through a buffer, so what is checked is
// a number rather than a picture.
auto tRenderedTextureReachesTheKernel =
    test("Crossing/aRenderedTextureIsReadByTheKernel") = []
{
    if (!computeIsAvailable())
        return;

    auto& device = Device::shared();
    auto view = RenderThenKernelView {};

    if (!view.target.isRenderTarget() || !view.readBack.isValid())
        return;

    renderOnce(view);

    auto green = 0.f;
    view.readBack.read(&green, sizeof(green));

    check(std::abs(green - 0.75f) < 0.1f,
          "the kernel read the colour the pass left in the target");

    if (!deviceCrossesResources())
    {
        check(device.crossingBytesThisFrame() == 0);
        return;
    }

    check(device.crossingsThisFrame() >= 1);
    check(device.crossingBytesThisFrame() >= imageBytes,
          "a pass names no rectangle either, so the target crossed whole");
};

// The one write that does report a rectangle: a host update of one. Only those
// texels are owed to the other side, and only those cross - against the whole
// resource when the host rewrote the lot.
auto tHostRegionCrossesAsARectangle =
    test("Crossing/aDirtyRectangleCrossesInsteadOfTheWholeTexture") = []
{
    if (!computeIsAvailable())
        return;

    auto& device = Device::shared();
    auto view = HostWriteThenKernelView {};

    if (!view.source.isValid() || !view.readBack.isValid())
        return;

    // A rectangle over the texel the kernel samples, so what comes back says
    // the rectangle landed where it was asked for and not merely that some
    // bytes moved.
    std::uint8_t region[regionBytes] = {};

    for (auto texel = 0; texel < regionSize * regionSize; ++texel)
    {
        region[texel * 4 + 1] = 191;
        region[texel * 4 + 3] = 255;
    }

    const auto centre = (float) (imageWidth / 2 - regionSize / 2);

    view.source.update({centre, centre, (float) regionSize, (float) regionSize},
                       region);

    renderOnce(view);

    auto green = 0.f;
    view.readBack.read(&green, sizeof(green));

    check(std::abs(green - 0.75f) < 0.1f, "the kernel sampled what the host wrote");

    if (!deviceCrossesResources())
    {
        check(device.crossingBytesThisFrame() == 0);
        return;
    }

    check(device.crossingBytesThisFrame() == regionBytes,
          "the rectangle crossed and not the texture");

    // And the whole resource when the host wrote the whole resource.
    std::uint8_t whole[imageBytes] = {};

    for (auto texel = 0; texel < imageWidth * imageHeight; ++texel)
    {
        whole[texel * 4 + 1] = 191;
        whole[texel * 4 + 3] = 255;
    }

    view.source.update(whole);
    renderOnce(view);

    check(device.crossingBytesThisFrame() == imageBytes,
          "an update with no rectangle owes the other side all of it");
};

// The counters are the frame's, so a second frame that crosses the same thing
// reports the same number rather than twice it.
auto tCountersAreClearedEachFrame =
    test("Crossing/theCountersAreClearedEachFrame") = []
{
    if (!computeIsAvailable())
        return;

    auto& device = Device::shared();
    auto view = KernelImageView {};

    if (!view.target.isValid())
        return;

    renderOnce(view);

    const auto first = device.crossingBytesThisFrame();
    const auto firstCount = device.crossingsThisFrame();

    renderOnce(view);

    check(device.crossingBytesThisFrame() == first,
          "the second frame reports its own crossing, not the pair");
    check(device.crossingsThisFrame() == firstCount);

    check((first > 0) == deviceCrossesResources(),
          "and the number is zero on exactly the devices that have one "
          "backend");
};

// A storage bind on a render stage where the device has no storage buffers is
// refused, and on a composite that means refused before anything is copied: a
// kernel's output dragged the whole way across to be dropped by the bind is the
// one crossing worth never paying (D11). The view's only crossing candidate is
// that buffer, so the counters answer the question outright.
auto tStorageBindIsRefusedRatherThanCrossed =
    test("Crossing/aRefusedStorageBindCostsNoCrossing") = []
{
    if (!computeIsAvailable())
        return;

    auto& device = Device::shared();
    auto view = KernelStorageBufferView {};

    if (!view.palette.isValid())
        return;

    auto image = renderOnce(view);

    check(image.isValid());

    if (!deviceCrossesResources())
    {
        check(device.crossingBytesThisFrame() == 0);
        return;
    }

    if (!device.supportsStorageBuffers())
    {
        check(device.crossingBytesThisFrame() == 0,
              "a bind the render half cannot make costs no copy at all");
        return;
    }

    check(device.crossingBytesThisFrame() >= (std::int64_t) (sizeof(float) * 3),
          "and where it can make it, the buffer crosses for it");

    auto pixel = image.at(viewWidth / 2, viewHeight / 2);

    check(std::abs(pixel.r - 0.25f) < 0.1f);
    check(std::abs(pixel.g - 0.5f) < 0.1f);
    check(std::abs(pixel.b - 0.75f) < 0.1f);
};
