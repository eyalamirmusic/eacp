#include "CpuCrossCheck.h"

#include <eacp/GPU/CpuCompute/Helpers.h>

#include <cstdint>
#include <functional>
#include <string>

// WGSL's structs on a device, every kernel cross-checked against the CPU
// executor field for field: a buffer of them read and written at
// StructLayout's offsets - every value type among the fields, three-vectors
// and every matrix shape beside scalars, f16 members, a nested struct - a
// whole element written from itself, a struct of halves alone whose elements
// share words between threads, struct variables and private arrays of them,
// and a fragment reading a buffer of structs. Each dialect lays the struct out
// its own way (ShaderStructEmission.h), so agreeing with the CPU here is
// agreeing with StructLayout.
//
// The CPU halves never self-skip; the GPU halves self-skip without a device,
// and those with an f16 field on one whose Device::supportsShaderFloat16
// answers no.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace StructDeviceTests
{
namespace
{
constexpr auto threadCount = 75;
constexpr auto targetSize = 4;
constexpr auto pixelCount = targetSize * targetSize;

struct Particle
{
    Float3 position;
    Float mass;
    Float2 velocity;
    UInt flags;

    EACP_SHADER_STRUCT(Particle, position, mass, velocity, flags)
};

struct Inner
{
    Float2 xy;
    Half h;

    EACP_SHADER_STRUCT(Inner, xy, h)
};

struct Outer
{
    Float a;
    Inner inner;
    Float3 v;
    Half2x2 hm;
    Float2x3 m;
    Int3 cell;

    EACP_SHADER_STRUCT(Outer, a, inner, v, hm, m, cell)
};

struct Halves
{
    Half value;

    EACP_SHADER_STRUCT(Halves, value)
};

struct Flagged
{
    Bool on;
    Float weight;

    EACP_SHADER_STRUCT(Flagged, on, weight)
};

// Every value type a buffer holds, each at the offset its alignment gives it
// after the one before: 464 bytes, with gaps after s, v3, v2, i3, m32 and i.
struct AllTypes
{
    Float s;
    Float3 v3;
    Float2 v2;
    Int3 i3;
    UInt u;
    Float4 v4;
    Float3x3 m33;
    Float2x3 m23;
    Float3x2 m32;
    Float4x3 m43;
    Int i;
    Float2x2 m22;
    UInt2 u2;
    Float4x4 m44;
    Float2x4 m24;
    Float3x4 m34;
    Float4x2 m42;

    EACP_SHADER_STRUCT(AllTypes,
                       s,
                       v3,
                       v2,
                       i3,
                       u,
                       v4,
                       m33,
                       m23,
                       m32,
                       m43,
                       i,
                       m22,
                       u2,
                       m44,
                       m24,
                       m34,
                       m42)
};

static_assert(StructLayout<AllTypes>::size == 464);
static_assert(StructLayout<AllTypes>::offsetOf<&AllTypes::m43>() == 192);
static_assert(StructLayout<AllTypes>::offsetOf<&AllTypes::m42>() == 432);

struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace StructDeviceTests

EACP_SHADER_VALUE(StructDeviceTests::QuadVertex, Float2)

namespace StructDeviceTests
{
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

float rounded(float value)
{
    return CpuCompute::quantizeToF16(value);
}

struct MoveKernel final : ComputeProgram
{
    MoveKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto p = input[id];

        write(output,
              id,
              Particle {p.position + float3(p.velocity, 1.f),
                        p.mass * 0.5f,
                        p.velocity.yx(),
                        p.flags | 1u});
        write(output, id, &Particle::mass, output[id].mass + 1.f);
    }

    Uniform<StructInputBuffer<Particle>> input;
    Uniform<StructOutputBuffer<Particle>> output;
    EACP_SHADER(input, output)
};

struct SwapKernel final : ComputeProgram
{
    SwapKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto o = values[id];
        auto inner = Inner {float2(o.a, o.inner.xy.y()), o.inner.h * 2.f};

        write(
            values,
            id,
            Outer {o.inner.xy.x(), inner, o.v * 2.f, o.hm * 0.5f, o.m, o.cell - 3});
        write(
            values, id, &Outer::inner, Inner {values[id].inner.xy.yx(), o.inner.h});
    }

    Uniform<StructOutputBuffer<Outer>> values;
    EACP_SHADER(values)
};

struct HalvesKernel final : ComputeProgram
{
    HalvesKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        write(halves, id, Halves {toHalf(toFloat(id)) * 0.5f + 0.25f});
    }

    Uniform<StructOutputBuffer<Halves>> halves;
    EACP_SHADER(halves)
};

struct VarsKernel final : ComputeProgram
{
    VarsKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto p = var(input[id]);
        p.set(&Particle::mass, p.get().mass * 2.f);

        auto list = privateArray<Particle, 4>();
        list.set(1, p.get());
        list.set(toInt(id), &Particle::velocity, p.get().velocity.yx());

        auto flag = var(Flagged {id < 3u, input[id].mass});
        flag.set(&Flagged::weight, flag.get().weight + 1.f);

        auto chosen = list[toInt(id) & 3];
        write(output,
              id,
              select(flag.get().on,
                     chosen.mass + flag.get().weight,
                     chosen.velocity.y() + toFloat(list[1].flags)));
    }

    Uniform<StructInputBuffer<Particle>> input;
    Uniform<OutputBuffer> output;
    EACP_SHADER(input, output)
};

struct AllTypesKernel final : ComputeProgram
{
    AllTypesKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto t = input[id];

        write(output,
              id,
              AllTypes {t.s * 2.f,
                        t.v3 * 2.f,
                        t.v2 * 2.f,
                        t.i3 + 1,
                        t.u + 1u,
                        t.v4 * 2.f,
                        t.m33 * 2.f,
                        t.m23 * 2.f,
                        t.m32 * 2.f,
                        t.m43 * 2.f,
                        t.i + 1,
                        t.m22 * 2.f,
                        t.u2 + 1u,
                        t.m44 * 2.f,
                        t.m24 * 2.f,
                        t.m34 * 2.f,
                        t.m42 * 2.f});
    }

    Uniform<StructInputBuffer<AllTypes>> input;
    Uniform<StructOutputBuffer<AllTypes>> output;
    EACP_SHADER(input, output)
};

struct FragmentShader final : ShaderProgram
{
    FragmentShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto column = toUInt(floor(fragmentCoordinate().x()));
        auto p = particles[column];
        setFragment(float4(p.position.y() + p.mass, 0.f, 0.f, 1.f));
    }

    Uniform<StructInputBuffer<Particle>> particles;
    EACP_SHADER(particles)
};

StructBufferData<Particle> particles(int count)
{
    auto data = StructBufferData<Particle> {count};

    for (auto i = 0; i < count; ++i)
    {
        auto f = (float) i;
        data.set<&Particle::position>(i, {f, f * 0.5f, -f});
        data.set<&Particle::mass>(i, 1.f + f * 0.25f);
        data.set<&Particle::velocity>(i, {0.5f, -f});
        data.set<&Particle::flags>(i, (std::uint32_t) i * 2u);
    }

    return data;
}

StructBufferData<Outer> outers(int count)
{
    auto data = StructBufferData<Outer> {count};

    for (auto i = 0; i < count; ++i)
    {
        auto f = (float) i;
        data.set<&Outer::a>(i, f);
        data.set<&Outer::inner, &Inner::xy>(i, {f + 0.5f, -f});
        data.set<&Outer::inner, &Inner::h>(i, 0.25f * f);
        data.set<&Outer::v>(i, {1.f, f, 2.f});
        data.set<&Outer::hm>(i, {1.f, 2.f, f, -4.f});
        data.set<&Outer::m>(i, {1.f, 2.f, 3.f, f, 5.f, 6.f});
        data.set<&Outer::cell>(i, {(std::int32_t) i, -1, 7});
    }

    return data;
}

// Component c of a float member of element i: distinct, and exact doubled.
float allTypesFloat(int element, int member, int component)
{
    return (float) (element * 64 + member * 17 + component) * 0.25f - 30.f;
}

template <auto Member>
void setFloats(StructBufferData<AllTypes>& data, int element, int member)
{
    auto value = StructHostValue<detail::MemberTypeOf<Member>> {};

    if constexpr (std::is_same_v<decltype(value), float>)
        value = allTypesFloat(element, member, 0);
    else
        for (auto c = 0; c < value.size(); ++c)
            value[c] = allTypesFloat(element, member, c);

    data.set<Member>(element, value);
}

template <auto Member>
bool doubledFloats(const StructBufferData<AllTypes>& data, int element, int member)
{
    auto value = data.get<Member>(element);

    if constexpr (std::is_same_v<decltype(value), float>)
        return value == 2.f * allTypesFloat(element, member, 0);
    else
        for (auto c = 0; c < value.size(); ++c)
            if (value[c] != 2.f * allTypesFloat(element, member, c))
                return false;

    return true;
}

StructBufferData<AllTypes> allTypes(int count)
{
    auto data = StructBufferData<AllTypes> {count};

    for (auto i = 0; i < count; ++i)
    {
        setFloats<&AllTypes::s>(data, i, 0);
        setFloats<&AllTypes::v3>(data, i, 1);
        setFloats<&AllTypes::v2>(data, i, 2);
        data.set<&AllTypes::i3>(i, {-i, i * 3, 7});
        data.set<&AllTypes::u>(i, (std::uint32_t) i * 5u);
        setFloats<&AllTypes::v4>(data, i, 5);
        setFloats<&AllTypes::m33>(data, i, 6);
        setFloats<&AllTypes::m23>(data, i, 7);
        setFloats<&AllTypes::m32>(data, i, 8);
        setFloats<&AllTypes::m43>(data, i, 9);
        data.set<&AllTypes::i>(i, -i * 2);
        setFloats<&AllTypes::m22>(data, i, 11);
        data.set<&AllTypes::u2>(i, {(std::uint32_t) i, 0xfffffff0u});
        setFloats<&AllTypes::m44>(data, i, 13);
        setFloats<&AllTypes::m24>(data, i, 14);
        setFloats<&AllTypes::m34>(data, i, 15);
        setFloats<&AllTypes::m42>(data, i, 16);
    }

    return data;
}

TextureDescriptor describeFloatTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::R32Float;
    descriptor.renderTarget = true;
    return descriptor;
}

RenderPipelineDescriptor describeFloatPipeline()
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::R32Float);
    return descriptor;
}

struct FrameRunner final : GPUView
{
    FrameRunner() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        draw(frame);
        frame.beginPass({Graphics::Color::black()});
    }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

using FloatPixels = Array<float, pixelCount>;

FloatPixels drawAndRead(const std::function<void(RenderPass&)>& draw)
{
    auto target = Device::shared().makeTexture(describeFloatTarget());
    auto pixels = FloatPixels {};
    auto view = FrameRunner {};

    auto drawFrame = [&](Frame& frame)
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = {-1.f, 0.f, 0.f, 1.f};

            auto pass = frame.beginPass(target, descriptor);
            draw(pass);
        }

        frame.flush();
        target.read(pixels.data());
    };

    view.draw = drawFrame;
    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    check(view.renderToImage(1.f).isValid());
    return pixels;
}
} // namespace

auto tMove = test("StructDevice/aBufferOfStructsIsReadAndWrittenAtItsLayout") = []
{
    auto kernel = MoveKernel {};
    auto input = particles(threadCount);
    auto output = StructBufferData<Particle> {threadCount};

    auto verify = [&](const Readback& readback)
    {
        auto moved = readback.structs(kernel.output);
        auto agreeing = 0;

        for (auto i = 0; i < threadCount; ++i)
        {
            auto f = (float) i;
            auto position = moved.get<&Particle::position>(i);
            auto velocity = moved.get<&Particle::velocity>(i);

            if (position[0] == f + 0.5f && position[1] == f * 0.5f - f
                && position[2] == -f + 1.f
                && moved.get<&Particle::mass>(i) == (1.f + f * 0.25f) * 0.5f + 1.f
                && velocity[0] == -f && velocity[1] == 0.5f
                && moved.get<&Particle::flags>(i) == ((std::uint32_t) i * 2u | 1u))
                ++agreeing;
        }

        check(agreeing == threadCount, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.input, input)
        .output(kernel.output, output)
        .agreeing()
        .run(threadCount, verify);
};

auto tAllTypes = test("StructDevice/everyValueTypeCrossesAtItsOffset") = []
{
    auto kernel = AllTypesKernel {};
    auto input = allTypes(threadCount);
    auto output = StructBufferData<AllTypes> {threadCount};

    auto verify = [&](const Readback& readback)
    {
        auto doubled = readback.structs(kernel.output);
        auto agreeing = 0;

        for (auto i = 0; i < threadCount; ++i)
        {
            auto i3 = doubled.get<&AllTypes::i3>(i);
            auto u2 = doubled.get<&AllTypes::u2>(i);
            auto integers =
                i3[0] == -i + 1 && i3[1] == i * 3 + 1 && i3[2] == 8
                && doubled.get<&AllTypes::u>(i) == (std::uint32_t) i * 5u + 1u
                && doubled.get<&AllTypes::i>(i) == -i * 2 + 1
                && u2[0] == (std::uint32_t) i + 1u && u2[1] == 0xfffffff1u;
            auto floats = doubledFloats<&AllTypes::s>(doubled, i, 0)
                          && doubledFloats<&AllTypes::v3>(doubled, i, 1)
                          && doubledFloats<&AllTypes::v2>(doubled, i, 2)
                          && doubledFloats<&AllTypes::v4>(doubled, i, 5)
                          && doubledFloats<&AllTypes::m33>(doubled, i, 6)
                          && doubledFloats<&AllTypes::m23>(doubled, i, 7)
                          && doubledFloats<&AllTypes::m32>(doubled, i, 8)
                          && doubledFloats<&AllTypes::m43>(doubled, i, 9)
                          && doubledFloats<&AllTypes::m22>(doubled, i, 11)
                          && doubledFloats<&AllTypes::m44>(doubled, i, 13)
                          && doubledFloats<&AllTypes::m24>(doubled, i, 14)
                          && doubledFloats<&AllTypes::m34>(doubled, i, 15)
                          && doubledFloats<&AllTypes::m42>(doubled, i, 16);

            if (integers && floats)
                ++agreeing;
        }

        check(agreeing == threadCount, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.input, input)
        .output(kernel.output, output)
        .agreeing()
        .run(threadCount, verify);
};

auto tNested =
    test("StructDevice/nestedStructsAndHalvesWriteAWholeElementFromItself") = []
{
    auto kernel = SwapKernel {};
    auto values = outers(threadCount);

    auto verify = [&](const Readback& readback)
    {
        auto swapped = readback.structs(kernel.values);
        auto agreeing = 0;

        for (auto i = 0; i < threadCount; ++i)
        {
            auto f = (float) i;
            auto xy = swapped.get<&Outer::inner, &Inner::xy>(i);
            auto hm = swapped.get<&Outer::hm>(i);
            auto m = swapped.get<&Outer::m>(i);
            auto cell = swapped.get<&Outer::cell>(i);
            auto v = swapped.get<&Outer::v>(i);

            if (swapped.get<&Outer::a>(i) == f + 0.5f && xy[0] == -f && xy[1] == f
                && swapped.get<&Outer::inner, &Inner::h>(i) == rounded(0.5f * f)
                && v[0] == 2.f && v[1] == 2.f * f && v[2] == 4.f && hm[0] == 0.5f
                && hm[1] == 1.f && hm[2] == rounded(0.5f * f) && hm[3] == -2.f
                && m[0] == 1.f && m[3] == f && m[5] == 6.f && cell[0] == i - 3
                && cell[1] == -4 && cell[2] == 4)
                ++agreeing;
        }

        check(agreeing == threadCount, readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.values, values)
        .agreeing()
        .run(threadCount, verify);
};

auto tHalves =
    test("StructDevice/aStructOfHalvesAloneSharesWordsBetweenThreads") = []
{
    auto kernel = HalvesKernel {};
    auto halves = StructBufferData<Halves> {threadCount + 1};

    for (auto i = 0; i <= threadCount; ++i)
        halves.set<&Halves::value>(i, -7.f);

    auto verify = [&](const Readback& readback)
    {
        auto written = readback.structs(kernel.halves);
        auto agreeing = 0;

        for (auto i = 0; i < threadCount; ++i)
            if (written.get<&Halves::value>(i) == rounded((float) i * 0.5f + 0.25f))
                ++agreeing;

        check(agreeing == threadCount, readback.name());
        check(written.get<&Halves::value>(threadCount) == -7.f,
              std::string(readback.name()) + ": the element past the grid is left");
    };

    CrossCheck {kernel}
        .output(kernel.halves, halves)
        .agreeing()
        .run(threadCount, verify);
};

auto tVars = test("StructDevice/variablesAndPrivateArraysHoldStructs") = []
{
    auto kernel = VarsKernel {};
    auto input = particles(threadCount);

    auto verify = [&](const Readback& readback)
    {
        const auto& output = readback.floats(kernel.output);
        auto agreeing = 0;

        for (auto i = 0; i < threadCount; ++i)
        {
            auto f = (float) i;
            auto mass = 1.f + f * 0.25f;
            auto slot = i & 3;
            auto chosenMass = slot == 1 ? mass * 2.f : 0.f;
            auto chosenY = slot == std::min(i, 3) ? 0.5f : (slot == 1 ? -f : 0.f);
            auto expected =
                i < 3 ? chosenMass + (mass + 1.f) : chosenY + (float) (i * 2);

            if (output[i] == expected)
                ++agreeing;
        }

        check(agreeing == threadCount, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.input, input)
        .output(kernel.output, threadCount, -1.f)
        .agreeing()
        .run(threadCount, verify);
};

auto tFragment = test("StructDevice/aFragmentReadsABufferOfStructs") = []
{
    if (!Device::shared().isValid())
        return;

    auto data = particles(targetSize);
    auto bytes = data.bytes();
    auto buffer = Device::shared().makeBuffer(
        bytes.data(), (std::int64_t) bytes.size(), BufferUsage::Storage);

    auto program = FragmentShader {};
    program.particles = buffer;
    program.setVertices(fullQuad);
    program.prepare(describeFloatPipeline());
    check(program.pipeline().isValid());

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    auto pixels = drawAndRead(draw);

    for (auto i = 0; i < pixelCount; ++i)
    {
        auto column = (float) (i % targetSize);
        auto expected = column * 0.5f + (1.f + column * 0.25f);
        check(pixels[i] == expected, std::to_string(pixels[i]));
    }
};
} // namespace StructDeviceTests
