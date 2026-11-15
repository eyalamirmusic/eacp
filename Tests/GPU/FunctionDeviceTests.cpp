#include "CpuCrossCheck.h"

#include <eacp/GPU/CpuCompute/Helpers.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

// WGSL's user functions on a device, every kernel cross-checked against the
// CPU executor, which inlines what the GPU calls: an early return from an if,
// a loop and a switch with lanes taking each, a function calling another,
// calls in a loop's condition and in a continuing block's break-if, a void
// function storing and running an atomic, a value function reading what the
// kernel wrote, a struct and a matrix in and out, and f16 values. Two render
// programs: one function called from both stages, a uniform read through a
// nested call; and a struct-returning call made in vertexStage beside a
// fragment function that discards.
//
// The CPU halves never self-skip; the GPU halves self-skip without a device,
// and the f16 one on a device whose Device::supportsShaderFloat16 answers no.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace FunctionDeviceTests
{
namespace
{
constexpr auto threadCount = 75;
constexpr auto sentinel = 0xdeadbeefu;
constexpr auto floatSentinel = -7.f;
constexpr auto halfSentinel = std::uint16_t {0x7e00u};
constexpr auto tolerance = 1e-5f;
constexpr auto targetSize = 4;
constexpr auto pixelCount = targetSize * targetSize;

struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace FunctionDeviceTests

EACP_SHADER_VALUE(FunctionDeviceTests::QuadVertex, Float2)

namespace FunctionDeviceTests
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

float inputAt(int index)
{
    return 0.125f * (float) index - 3.f;
}

Vector<float> inputs()
{
    auto values = Vector<float> {};

    for (auto index = 0; index < threadCount; ++index)
        values.add(inputAt(index));

    return values;
}

bool isNear(float value, float expected)
{
    return std::abs(value - expected) <= tolerance;
}

struct BlendKernel final : ComputeProgram
{
    BlendKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto clampUnit = function<Float(Float)>("clampUnit",
                                                [&](Float x)
                                                {
                                                    auto belowZero = [&]
                                                    { returnEarly(constant(0.f)); };
                                                    ifThen(x < 0.f, belowZero);
                                                    return min(x, 1.f);
                                                });

        auto blend = function<Float(Float, Float)>(
            "blend",
            [&](Float a, Float b)
            { return clampUnit(a) * 0.25f + clampUnit(b) * 0.75f; });

        auto x = input[id];
        write(output, id, blend(x, x * -0.5f + 1.f) + blend(x * 0.25f, x));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    EACP_SHADER(input, output)
};

float clampUnit(float x)
{
    return x < 0.f ? 0.f : std::min(x, 1.f);
}

float blend(float a, float b)
{
    return clampUnit(a) * 0.25f + clampUnit(b) * 0.75f;
}

float expectedBlend(int index)
{
    auto x = inputAt(index);
    return blend(x, x * -0.5f + 1.f) + blend(x * 0.25f, x);
}

struct SearchKernel final : ComputeProgram
{
    SearchKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto firstSquareAbove = function<UInt(UInt, UInt)>(
            "firstSquareAbove",
            [&](UInt start, UInt limit)
            {
                auto at = var(start);
                auto found = [&] { returnEarly(at.get()); };
                auto special = [&] { returnEarly(unsignedInteger(500u)); };
                auto step = [&]
                {
                    ifThen(at.get() * at.get() > limit, found);
                    switchOn(at.get(), caseOf(9, special));
                    at += 1u;
                };

                loop(at.get() < 12u, step);
                return unsignedInteger(999u);
            });

        auto addOne = function<UInt(UInt)>("addOne", [&](UInt x) { return x + 1u; });

        auto count = var(0u);
        auto advance = [&] { count += 1u; };
        loop(addOne(count.get()) < id % 13u, advance);

        auto counter = var(0u);
        auto total = var(0u);
        auto body = [&] { total += addOne(counter.get()); };
        auto update = [&]
        {
            counter += 1u;
            breakLoopIf(addOne(counter.get()) > id % 7u + 1u);
        };
        loop(counter.get() < 20u, body, update);

        write(found, id, firstSquareAbove(id % 5u, id * 3u));
        write(counts, id, count.get() + total.get() * 100u + counter.get() * 10000u);
    }

    Uniform<UIntOutputBuffer> found;
    Uniform<UIntOutputBuffer> counts;
    EACP_SHADER(found, counts)
};

std::uint32_t firstSquareAbove(std::uint32_t start, std::uint32_t limit)
{
    for (auto at = start; at < 12u; ++at)
    {
        if (at * at > limit)
            return at;

        if (at == 9u)
            return 500u;
    }

    return 999u;
}

std::uint32_t expectedCounts(std::uint32_t id)
{
    auto count = 0u;

    while (count + 1u < id % 13u)
        ++count;

    auto counter = 0u;
    auto total = 0u;

    while (counter < 20u)
    {
        total += counter + 1u;
        counter += 1u;

        if (counter + 1u > id % 7u + 1u)
            break;
    }

    return count + total * 100u + counter * 10000u;
}

struct RecordKernel final : ComputeProgram
{
    RecordKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto record =
            function<void(UInt, Float)>("record",
                                        [&](UInt at, Float value)
                                        {
                                            write(first, at, value);
                                            atomicAdd(counter, 0u, 1u);

                                            auto leave = [&] { returnEarly(); };
                                            ifThen(value > 0.5f, leave);
                                            write(second, at, value * 2.f);
                                        });

        auto readBack = function<Float(UInt)>(
            "readBack", [&](UInt at) { return first[at] + 1.f; });

        record(id, input[id]);
        write(third, id, readBack(id));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> first;
    Uniform<OutputBuffer> second;
    Uniform<OutputBuffer> third;
    Uniform<AtomicBuffer> counter;
    EACP_SHADER(input, first, second, third, counter)
};

struct Body
{
    Float3 position;
    Float mass;

    EACP_SHADER_STRUCT(Body, position, mass)
};

struct StructKernel final : ComputeProgram
{
    StructKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto weigh = function<Body(Body, Float)>(
            "weigh",
            [&](Body body, Float k)
            { return Body {body.position * k, body.mass + k}; });

        auto turn = function<Float2(Float2x2, Float2)>(
            "turn", [&](Float2x2 m, Float2 v) { return m * v; });

        auto k = toFloat(id);
        write(bodies, id, weigh(input[id], k));

        auto rotation =
            float2x2(float2(constant(0.f), 1.f), float2(constant(-1.f), 0.f));
        auto turned = turn(rotation, float2(k, 1.f));
        write(turns, id, turned.x() * 10.f + turned.y());
    }

    Uniform<StructInputBuffer<Body>> input;
    Uniform<StructOutputBuffer<Body>> bodies;
    Uniform<OutputBuffer> turns;
    EACP_SHADER(input, bodies, turns)
};

struct HalfKernel final : ComputeProgram
{
    HalfKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto mixHalf = function<Half(Half, Half2)>(
            "mixHalf", [&](Half h, Half2 v) { return h * v.x() + v.y(); });

        auto h = halves[id];
        write(output, id, mixHalf(h, half2(h, toHalf(toFloat(id) * 0.25f))));
    }

    Uniform<HalfInputBuffer> halves;
    Uniform<HalfOutputBuffer> output;
    EACP_SHADER(halves, output)
};

// squeeze moves the quad to the left half; shade, given the pixel's column,
// returns early on the first and reads the uniform through squeeze on the
// second.
struct BothStagesShader final : ShaderProgram
{
    BothStagesShader() { compile(); }

    void define() override
    {
        auto squeeze = function<Float(Float)>(
            "squeeze", [&](Float x) { return x * 0.5f - 0.5f; });

        auto shade = function<Float(Float)>("shade",
                                            [&](Float x)
                                            {
                                                auto first = [&]
                                                { returnEarly(constant(0.25f)); };
                                                ifThen(x < 1.f, first);
                                                return squeeze(gain) + 0.75f;
                                            });

        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(squeeze(position.x()), position.y(), 0.f, 1.f));
        setFragment(float4(shade(fragmentCoordinate().x()), 0.f, 0.f, 1.f));
    }

    Uniform<Float> gain;
    EACP_SHADER(gain)
};

struct Lifted
{
    Float2 xy;
    Float depth;

    EACP_SHADER_STRUCT(Lifted, xy, depth)
};

// lift moves the quad to the right half from vertexStage; dropTopRow discards
// the fragments of the top row.
struct VertexCallShader final : ShaderProgram
{
    VertexCallShader() { compile(); }

    void define() override
    {
        auto lift = function<Lifted(Float2)>(
            "lift",
            [&](Float2 p)
            {
                return Lifted {float2(p.x() * 0.5f + 0.5f, p.y()), constant(0.25f)};
            });

        auto dropTopRow = function<void(Float)>("dropTopRow",
                                                [&](Float y)
                                                {
                                                    auto drop = [&] { discard(); };
                                                    ifThen(y < 1.f, drop);
                                                });

        auto position = vertexInput(&QuadVertex::position);
        auto place = [&]
        {
            auto lifted = lift(position);
            setPosition(float4(lifted.xy, lifted.depth, 1.f));
        };

        vertexStage(place);
        dropTopRow(fragmentCoordinate().y());
        setFragment(float4(constant(1.f), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

bool canTest()
{
    return Device::shared().isValid();
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

// One pass into a fresh R32Float target cleared to -1, read back.
FloatPixels drawAndRead(const std::function<void(RenderPass&)>& draw)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::R32Float;
    descriptor.renderTarget = true;

    auto target = Device::shared().makeTexture(descriptor);
    auto pixels = FloatPixels {};
    auto view = FrameRunner {};

    auto drawFrame = [&](Frame& frame)
    {
        {
            auto pass = RenderPassDescriptor {};
            pass.clearColor = {-1.f, 0.f, 0.f, 1.f};

            auto encoder = frame.beginPass(target, pass);
            draw(encoder);
        }

        frame.flush();
        target.read(pixels.data());
    };

    view.draw = drawFrame;
    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    check(view.renderToImage(1.f).isValid());
    return pixels;
}

bool near(float value, float expected)
{
    return std::abs(value - expected) < 0.002f;
}
} // namespace

auto tBlend = test("FunctionDevice/nestedCallsReturnEarlyLikeTheCpu") = []
{
    auto kernel = BlendKernel {};
    auto verify = [&](const Readback& readback)
    {
        const auto& output = readback.floats(kernel.output);
        auto mismatches = 0;

        for (auto index = 0; index < threadCount; ++index)
            mismatches += isNear(output[index], expectedBlend(index)) ? 0 : 1;

        check(mismatches == 0, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.input, inputs())
        .output(kernel.output, threadCount, floatSentinel)
        .agreeing(tolerance)
        .run(threadCount, verify);
};

auto tSearch = test("FunctionDevice/returnsAndCallsInConditionsAgree") = []
{
    auto kernel = SearchKernel {};
    auto verify = [&](const Readback& readback)
    {
        const auto& found = readback.uints(kernel.found);
        const auto& counts = readback.uints(kernel.counts);
        auto mismatches = 0;

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            mismatches += found[(int) id] != firstSquareAbove(id % 5u, id * 3u);
            mismatches += counts[(int) id] != expectedCounts(id);
        }

        check(mismatches == 0, readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.found, threadCount, sentinel)
        .output(kernel.counts, threadCount, sentinel)
        .agreeing()
        .run(threadCount, verify);
};

auto tRecord = test("FunctionDevice/voidFunctionsStoreAndCount") = []
{
    auto kernel = RecordKernel {};
    auto verify = [&](const Readback& readback)
    {
        const auto& first = readback.floats(kernel.first);
        const auto& second = readback.floats(kernel.second);
        const auto& third = readback.floats(kernel.third);
        auto mismatches = 0;

        for (auto index = 0; index < threadCount; ++index)
        {
            auto x = inputAt(index);
            mismatches += first[index] != x;
            mismatches += second[index] != (x > 0.5f ? floatSentinel : x * 2.f);
            mismatches += third[index] != x + 1.f;
        }

        check(mismatches == 0, readback.name());
        check(readback.uints(kernel.counter)[0] == (std::uint32_t) threadCount,
              readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.input, inputs())
        .output(kernel.first, threadCount, floatSentinel)
        .output(kernel.second, threadCount, floatSentinel)
        .output(kernel.third, threadCount, floatSentinel)
        .output(kernel.counter, 1)
        .agreeing()
        .run(threadCount, verify);
};

auto tStruct = test("FunctionDevice/structsAndMatricesPassByValue") = []
{
    auto kernel = StructKernel {};
    auto input = StructBufferData<Body> {threadCount};

    for (auto i = 0; i < threadCount; ++i)
    {
        auto f = (float) i;
        input.set<&Body::position>(i, {f, -f, 0.5f});
        input.set<&Body::mass>(i, 2.f + f);
    }

    auto verify = [&](const Readback& readback)
    {
        auto bodies = readback.structs(kernel.bodies);
        const auto& turns = readback.floats(kernel.turns);
        auto mismatches = 0;

        for (auto i = 0; i < threadCount; ++i)
        {
            auto f = (float) i;
            auto position = bodies.get<&Body::position>(i);

            mismatches += position[0] != f * f || position[1] != -f * f
                          || position[2] != 0.5f * f;
            mismatches += bodies.get<&Body::mass>(i) != 2.f + f + f;
            mismatches += turns[i] != -10.f + f;
        }

        check(mismatches == 0, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.input, input)
        .output(kernel.bodies, StructBufferData<Body> {threadCount})
        .output(kernel.turns, threadCount, floatSentinel)
        .agreeing()
        .run(threadCount, verify);
};

auto tHalf = test("FunctionDevice/halvesPassByValue") = []
{
    auto kernel = HalfKernel {};
    auto halves = Vector<std::uint16_t> {};

    for (auto i = 0; i < threadCount; ++i)
        halves.add(CpuCompute::narrowToHalf(inputAt(i) * 0.5f));

    auto verify = [&](const Readback& readback)
    {
        const auto& output = readback.halves(kernel.output);
        auto mismatches = 0;

        for (auto i = 0; i < threadCount; ++i)
        {
            auto h = CpuCompute::widenHalf(halves[i]);
            auto k =
                CpuCompute::widenHalf(CpuCompute::narrowToHalf((float) i * 0.25f));
            auto product = CpuCompute::widenHalf(CpuCompute::narrowToHalf(h * h));
            mismatches += output[i] != CpuCompute::narrowToHalf(product + k);
        }

        check(mismatches == 0, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.halves, halves)
        .output(kernel.output, filled(threadCount, halfSentinel))
        .agreeing()
        .run(threadCount, verify);
};

auto tBothStages = test("FunctionDevice/bothRenderStagesCallOneFunction") = []
{
    if (!canTest())
        return;

    auto program = BothStagesShader {};
    program.gain = 3.f;
    program.setVertices(fullQuad);
    program.prepare(describeFloatPipeline());

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    auto pixels = drawAndRead(draw);
    const float columns[] = {0.25f, 1.75f, -1.f, -1.f};

    for (auto i = 0; i < pixelCount; ++i)
        check(near(pixels[i], columns[i % targetSize]),
              std::to_string(i) + ": " + std::to_string(pixels[i]));
};

auto tVertexCall = test("FunctionDevice/aVertexStageCallAndAFragmentDiscard") = []
{
    if (!canTest())
        return;

    auto program = VertexCallShader {};
    program.setVertices(fullQuad);
    program.prepare(describeFloatPipeline());

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    auto pixels = drawAndRead(draw);

    for (auto i = 0; i < pixelCount; ++i)
    {
        auto drawn = i % targetSize >= targetSize / 2 && i / targetSize > 0;
        check(near(pixels[i], drawn ? 1.f : -1.f),
              std::to_string(i) + ": " + std::to_string(pixels[i]));
    }
};
} // namespace FunctionDeviceTests
