#include "CpuCrossCheck.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <source_location>
#include <string>

// The control flow of WGSL on a device. Kernels with a switch every lane takes
// differently, early returns, a continuing block with break-if, and switches
// nested in a loop whose clauses break and continue it run on the GPU and the
// CPU executor alike and must agree word for word. The render stages are read
// back from the pixels: a discard() in a switch drops whole columns, and under
// a depth test the dropped fragments leave the depth they did not write for a
// later quad to pass; an early fragment return writes the colour chosen before
// it; a vertex switch on vertexId() places a quad, and an early vertex return
// keeps a later statement from moving one.
//
// The kernels' CPU halves never self-skip; everything else self-skips without
// a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace ControlFlowDeviceTests
{
namespace
{
constexpr auto sentinel = 0xdeadbeefu;
constexpr auto threadCount = 300;
constexpr auto targetSize = 4;
constexpr auto pixelCount = targetSize * targetSize;

struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace ControlFlowDeviceTests

EACP_SHADER_VALUE(ControlFlowDeviceTests::QuadVertex, Float2)

namespace ControlFlowDeviceTests
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

constexpr QuadVertex twoQuads[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

struct SwitchKernel final : ComputeProgram
{
    SwitchKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto value = var(0u);

        auto first = [&] { value = 100u + id; };
        auto shared = [&] { value = 200u; };
        auto bump = [&] { value += 1u; };
        auto tripled = [&]
        {
            value = id * 3u;
            ifThen(id > 20u, bump);
        };
        auto rest = [&] { value = 900u + id % 7u; };

        switchOn(id % 7u,
                 caseOf(0, first),
                 caseOf({1, 4}, shared),
                 caseOf(2, tripled),
                 defaultCase(rest));

        auto signedValue = var(0);
        auto low = [&] { signedValue = -1; };
        auto high = [&] { signedValue = 5; };
        switchOn(toInt(id) - 8, caseOf({-8, -1}, low), caseOf(5, high));

        write(output, id, value.get());
        write(signedOutput, id, toUInt(signedValue.get()));
    }

    Uniform<UIntOutputBuffer> output;
    Uniform<UIntOutputBuffer> signedOutput;
    EACP_SHADER(output, signedOutput)
};

std::uint32_t expectedSwitch(std::uint32_t id)
{
    switch (id % 7u)
    {
        case 0u:
            return 100u + id;
        case 1u:
        case 4u:
            return 200u;
        case 2u:
            return id * 3u + (id > 20u ? 1u : 0u);
        default:
            return 900u + id % 7u;
    }
}

std::uint32_t expectedSignedSwitch(std::uint32_t id)
{
    auto selector = (int) id - 8;

    if (selector == -8 || selector == -1)
        return (std::uint32_t) -1;

    return selector == 5 ? 5u : 0u;
}

struct EarlyReturnKernel final : ComputeProgram
{
    EarlyReturnKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto leave = [&] { returnEarly(); };

        write(first, id, 1u);
        ifThen(id % 3u == 0u, leave);
        write(second, id, 2u);

        auto counter = var(0u);
        auto step = [&]
        {
            counter += 1u;
            ifThen(counter.get() == id % 5u + 1u && id % 2u == 1u, leave);
        };

        loop(counter.get() < 10u, step);
        write(third, id, counter.get());

        switchOn(id % 4u, caseOf(2, leave));
        write(fourth, id, 4u);
    }

    Uniform<UIntOutputBuffer> first;
    Uniform<UIntOutputBuffer> second;
    Uniform<UIntOutputBuffer> third;
    Uniform<UIntOutputBuffer> fourth;
    EACP_SHADER(first, second, third, fourth)
};

struct BreakIfKernel final : ComputeProgram
{
    BreakIfKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto counter = var(0u);
        auto total = var(0u);

        auto skip = [&] { continueLoop(); };
        auto body = [&]
        {
            ifThen(counter.get() % 3u == 0u, skip);
            total += counter.get();
        };

        auto update = [&]
        {
            counter += 1u;
            breakLoopIf(total.get() > id);
        };

        loop(counter.get() < 20u, body, update);
        write(totals, id, total.get());
        write(counters, id, counter.get());
    }

    Uniform<UIntOutputBuffer> totals;
    Uniform<UIntOutputBuffer> counters;
    EACP_SHADER(totals, counters)
};

struct Totals
{
    std::uint32_t total = 0;
    std::uint32_t counter = 0;
};

Totals expectedBreakIf(std::uint32_t id)
{
    auto result = Totals {};

    while (result.counter < 20u)
    {
        if (result.counter % 3u != 0u)
            result.total += result.counter;

        result.counter += 1u;

        if (result.total > id)
            break;
    }

    return result;
}

struct NestedSwitchKernel final : ComputeProgram
{
    NestedSwitchKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto counter = var(0u);
        auto total = var(0u);

        auto stop = [&] { breakLoop(); };
        auto skip = [&] { continueLoop(); };
        auto addLots = [&] { total += 1000u; };
        auto inner = [&]
        {
            switchOn(
                id % 3u, caseOf(0, stop), caseOf(1, skip), defaultCase(addLots));
        };
        auto addCounter = [&] { total += counter.get(); };
        auto maybeStop = [&] { ifThen(counter.get() > id % 11u, stop); };

        auto step = [&]
        {
            counter += 1u;
            switchOn(counter.get() % 4u,
                     caseOf(0, inner),
                     caseOf(1, addCounter),
                     caseOf(2, maybeStop),
                     defaultCase(skip));
            total += 1u;
        };

        loop(counter.get() < 12u, step);
        write(totals, id, total.get());
        write(counters, id, counter.get());
    }

    Uniform<UIntOutputBuffer> totals;
    Uniform<UIntOutputBuffer> counters;
    EACP_SHADER(totals, counters)
};

Totals expectedNestedSwitch(std::uint32_t id)
{
    auto result = Totals {};

    while (result.counter < 12u)
    {
        result.counter += 1u;
        auto remainder = result.counter % 4u;

        if (remainder == 0u)
        {
            if (id % 3u == 0u)
                break;

            if (id % 3u == 1u)
                continue;

            result.total += 1000u;
        }
        else if (remainder == 1u)
        {
            result.total += result.counter;
        }
        else if (remainder == 2u)
        {
            if (result.counter > id % 11u)
                break;
        }
        else
        {
            continue;
        }

        result.total += 1u;
    }

    return result;
}

// Columns 0 and 3 dropped by a switch on the pixel's column, the rest written
// as 1 at a depth of 0.25.
struct ColumnDiscardShader final : ShaderProgram
{
    ColumnDiscardShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.25f, 1.f));

        auto drop = [&] { discard(); };
        auto column = toUInt(fragmentCoordinate().x());
        switchOn(column, caseOf({0, 3}, drop));
        setFragment(float4(constant(1.f), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

// Written as 2 at a depth of 0.5.
struct FartherQuadShader final : ShaderProgram
{
    FartherQuadShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.5f, 1.f));
        setFragment(float4(constant(2.f), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

// 0.75 left of the middle, returned from inside the branch; 0.5 right of it,
// set after the branch.
struct EarlyFragmentShader final : ShaderProgram
{
    EarlyFragmentShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto shade = var(0.25f);
        auto leave = [&]
        {
            shade = 0.75f;
            returnEarly();
        };

        ifThen(fragmentCoordinate().x() < (float) targetSize / 2.f, leave);
        shade = 0.5f;
        setFragment(float4(shade.get(), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

// The quad's left half, each corner's x picked by a switch on vertexId().
struct VertexSwitchShader final : ShaderProgram
{
    VertexSwitchShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        auto place = [&]
        {
            auto x = var(constant(5.f));
            auto outer = [&] { x = constant(-1.f); };
            auto middle = [&] { x = constant(0.f); };
            switchOn(
                vertexId(), caseOf({0, 2, 5}, outer), caseOf({1, 3, 4}, middle));
            setPosition(float4(x.get(), position.y(), 0.f, 1.f));
        };

        vertexStage(place);
        setFragment(float4(constant(1.f), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

// Two copies of the full quad: the first squeezed into the left half, the
// second into the right half and returned there - the statement after the
// return would throw it off screen.
struct VertexReturnShader final : ShaderProgram
{
    VertexReturnShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        auto place = [&]
        {
            auto x = var(position.x() * 0.5f - 0.5f);
            auto later = vertexId() >= 6u;
            auto keepRight = [&]
            {
                x = position.x() * 0.5f + 0.5f;
                returnEarly();
            };
            auto throwAway = [&] { x = constant(5.f); };

            ifThen(later, keepRight);
            ifThen(later, throwAway);
            setPosition(float4(x.get(), position.y(), 0.f, 1.f));
        };

        vertexStage(place);
        setFragment(float4(constant(1.f), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

bool canTest()
{
    return Device::shared().isValid();
}

TextureDescriptor describeFloatTarget(bool depth)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::R32Float;
    descriptor.renderTarget = true;
    descriptor.depth = depth;
    return descriptor;
}

RenderPipelineDescriptor describeFloatPipeline(bool depth)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::R32Float);
    descriptor.depth = depth;
    descriptor.depthCompare = DepthCompare::Less;
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
FloatPixels drawAndRead(bool depth, const std::function<void(RenderPass&)>& draw)
{
    auto target = Device::shared().makeTexture(describeFloatTarget(depth));
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

bool near(float value, float expected)
{
    return std::abs(value - expected) < 0.002f;
}

// Whether every pixel is `left` in the columns below `split` and `right` in the
// rest.
bool isSplitAt(const FloatPixels& pixels, int split, float left, float right)
{
    for (auto i = 0; i < pixelCount; ++i)
        if (!near(pixels[i], i % targetSize < split ? left : right))
            return false;

    return true;
}

template <typename Program>
void prepare(Program& program, bool depth)
{
    program.setVertices(fullQuad);
    program.prepare(describeFloatPipeline(depth));
}

// One shape of early return per kernel, each writing `before` ahead of it and
// `after` behind it, so a compiler that gets one shape wrong is named by the
// case that fails.
struct ReturnInLoopKernel final : ComputeProgram
{
    ReturnInLoopKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto counter = var(0u);
        auto leave = [&] { returnEarly(); };
        auto step = [&]
        {
            counter += 1u;
            ifThen(counter.get() == id % 5u + 1u && id % 2u == 1u, leave);
        };

        write(before, id, 1u);
        loop(counter.get() < 10u, step);
        write(after, id, counter.get());
    }

    Uniform<UIntOutputBuffer> before;
    Uniform<UIntOutputBuffer> after;
    EACP_SHADER(before, after)
};

struct ReturnInSwitchKernel final : ComputeProgram
{
    ReturnInSwitchKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto leave = [&] { returnEarly(); };

        write(before, id, 1u);
        switchOn(id % 4u, caseOf(2, leave));
        write(after, id, 4u);
    }

    Uniform<UIntOutputBuffer> before;
    Uniform<UIntOutputBuffer> after;
    EACP_SHADER(before, after)
};

struct ReturnInSwitchInLoopKernel final : ComputeProgram
{
    ReturnInSwitchInLoopKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto counter = var(0u);
        auto leave = [&] { returnEarly(); };
        auto step = [&]
        {
            counter += 1u;
            switchOn(counter.get() + id % 5u, caseOf(12, leave));
        };

        write(before, id, 1u);
        loop(counter.get() < 10u, step);
        write(after, id, counter.get());
    }

    Uniform<UIntOutputBuffer> before;
    Uniform<UIntOutputBuffer> after;
    EACP_SHADER(before, after)
};

struct FunctionReturnInLoopKernel final : ComputeProgram
{
    FunctionReturnInLoopKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto firstRootAbove =
            function<UInt(UInt)>("firstRootAbove",
                                 [&](UInt limit)
                                 {
                                     auto at = var(0u);
                                     auto found = [&] { returnEarly(at.get()); };
                                     auto step = [&]
                                     {
                                         ifThen(at.get() * at.get() > limit, found);
                                         at += 1u;
                                     };

                                     loop(at.get() < 12u, step);
                                     return unsignedInteger(999u);
                                 });

        write(before, id, 1u);
        write(after, id, firstRootAbove(id % 150u));
    }

    Uniform<UIntOutputBuffer> before;
    Uniform<UIntOutputBuffer> after;
    EACP_SHADER(before, after)
};

struct FunctionReturnInSwitchInLoopKernel final : ComputeProgram
{
    FunctionReturnInSwitchInLoopKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        auto reachesNine = function<UInt(UInt)>(
            "reachesNine",
            [&](UInt start)
            {
                auto at = var(start);
                auto nine = [&] { returnEarly(unsignedInteger(500u)); };
                auto step = [&]
                {
                    switchOn(at.get(), caseOf(9, nine));
                    at += 1u;
                };

                loop(at.get() < 12u, step);
                return unsignedInteger(999u);
            });

        write(before, id, 1u);
        write(after, id, reachesNine(id % 12u));
    }

    Uniform<UIntOutputBuffer> before;
    Uniform<UIntOutputBuffer> after;
    EACP_SHADER(before, after)
};

std::uint32_t firstRootAbove(std::uint32_t limit)
{
    for (auto at = 0u; at < 12u; ++at)
        if (at * at > limit)
            return at;

    return 999u;
}

// Every lane's before and after against what it should hold, the first wrong
// lane spelled out so a failing backend's log says what it wrote.
template <typename Kernel, typename Expected>
void checkReturnShape(
    const std::source_location& location = std::source_location::current())
{
    auto kernel = Kernel {};
    auto expectedAfter = Expected {};

    auto verify = [&](const Readback& readback)
    {
        const auto& before = readback.uints(kernel.before);
        const auto& after = readback.uints(kernel.after);
        auto wrong = 0;
        auto firstWrong = std::string {};

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            auto at = (int) id;
            auto expected = expectedAfter(id);

            if (before[at] == 1u && after[at] == expected)
                continue;

            if (wrong++ == 0)
                firstWrong = "lane " + std::to_string(id) + ": before "
                             + std::to_string(before[at]) + ", after "
                             + std::to_string(after[at]) + ", expected after "
                             + std::to_string(expected);
        }

        check(wrong == 0,
              std::string(readback.name()) + ": " + std::to_string(wrong) + " of "
                  + std::to_string(threadCount) + " lanes wrong; " + firstWrong,
              location);
    };

    CrossCheck {kernel}
        .output(kernel.before, threadCount, sentinel)
        .output(kernel.after, threadCount, sentinel)
        .agreeing()
        .run(threadCount, verify, location);
}

struct AfterReturnInLoop
{
    std::uint32_t operator()(std::uint32_t id) const
    {
        return id % 2u == 1u ? sentinel : 10u;
    }
};

struct AfterReturnInSwitch
{
    std::uint32_t operator()(std::uint32_t id) const
    {
        return id % 4u == 2u ? sentinel : 4u;
    }
};

struct AfterReturnInSwitchInLoop
{
    std::uint32_t operator()(std::uint32_t id) const
    {
        return id % 5u >= 2u ? sentinel : 10u;
    }
};

struct AfterFunctionReturnInLoop
{
    std::uint32_t operator()(std::uint32_t id) const
    {
        return firstRootAbove(id % 150u);
    }
};

struct AfterFunctionReturnInSwitchInLoop
{
    std::uint32_t operator()(std::uint32_t id) const
    {
        return id % 12u <= 9u ? 500u : 999u;
    }
};
} // namespace

auto tSwitch = test("ControlFlowDevice/eachLaneTakesItsOwnClause") = []
{
    auto kernel = SwitchKernel {};
    auto verify = [&](const Readback& readback)
    {
        const auto& output = readback.uints(kernel.output);
        const auto& signedOutput = readback.uints(kernel.signedOutput);
        auto mismatches = 0;

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            mismatches += output[(int) id] != expectedSwitch(id);
            mismatches += signedOutput[(int) id] != expectedSignedSwitch(id);
        }

        check(mismatches == 0, readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.output, threadCount, sentinel)
        .output(kernel.signedOutput, threadCount, sentinel)
        .agreeing()
        .run(threadCount, verify);
};

auto tEarlyReturn =
    test("ControlFlowDevice/aReturnSkipsTheLaterWritesOfItsLanes") = []
{
    auto kernel = EarlyReturnKernel {};
    auto verify = [&](const Readback& readback)
    {
        const auto& first = readback.uints(kernel.first);
        const auto& second = readback.uints(kernel.second);
        const auto& third = readback.uints(kernel.third);
        const auto& fourth = readback.uints(kernel.fourth);
        auto mismatches = 0;

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            auto at = (int) id;
            auto pastFirst = id % 3u != 0u;
            auto pastLoop = pastFirst && id % 2u == 0u;
            auto pastSwitch = pastLoop && id % 4u != 2u;

            mismatches += first[at] != 1u;
            mismatches += second[at] != (pastFirst ? 2u : sentinel);
            mismatches += third[at] != (pastLoop ? 10u : sentinel);
            mismatches += fourth[at] != (pastSwitch ? 4u : sentinel);
        }

        check(mismatches == 0, readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.first, threadCount, sentinel)
        .output(kernel.second, threadCount, sentinel)
        .output(kernel.third, threadCount, sentinel)
        .output(kernel.fourth, threadCount, sentinel)
        .agreeing()
        .run(threadCount, verify);
};

auto tReturnInLoop =
    test("ControlFlowDevice/aReturnInALoopAloneSkipsWhatFollows") = []
{ checkReturnShape<ReturnInLoopKernel, AfterReturnInLoop>(); };

auto tReturnInSwitch =
    test("ControlFlowDevice/aReturnAloneInASwitchClauseSkipsWhatFollows") = []
{ checkReturnShape<ReturnInSwitchKernel, AfterReturnInSwitch>(); };

auto tReturnInSwitchInLoop =
    test("ControlFlowDevice/aReturnInASwitchClauseInALoopSkipsWhatFollows") = []
{ checkReturnShape<ReturnInSwitchInLoopKernel, AfterReturnInSwitchInLoop>(); };

auto tFunctionReturnInLoop =
    test("ControlFlowDevice/aFunctionReturningFromALoopAnswersItsCaller") = []
{ checkReturnShape<FunctionReturnInLoopKernel, AfterFunctionReturnInLoop>(); };

auto tFunctionReturnInSwitchInLoop = test(
    "ControlFlowDevice/aFunctionReturningFromASwitchInALoopAnswersItsCaller") = []
{
    checkReturnShape<FunctionReturnInSwitchInLoopKernel,
                     AfterFunctionReturnInSwitchInLoop>();
};

auto tBreakIf = test("ControlFlowDevice/breakIfEndsEachLaneAfterItsContinuing") = []
{
    auto kernel = BreakIfKernel {};
    auto verify = [&](const Readback& readback)
    {
        const auto& totals = readback.uints(kernel.totals);
        const auto& counters = readback.uints(kernel.counters);
        auto mismatches = 0;

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            auto expected = expectedBreakIf(id);
            mismatches += totals[(int) id] != expected.total;
            mismatches += counters[(int) id] != expected.counter;
        }

        check(mismatches == 0, readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.totals, threadCount, sentinel)
        .output(kernel.counters, threadCount, sentinel)
        .agreeing()
        .run(threadCount, verify);
};

auto tNestedSwitch =
    test("ControlFlowDevice/clausesOfANestedSwitchBreakAndContinueTheLoop") = []
{
    auto kernel = NestedSwitchKernel {};
    auto verify = [&](const Readback& readback)
    {
        const auto& totals = readback.uints(kernel.totals);
        const auto& counters = readback.uints(kernel.counters);
        auto mismatches = 0;

        for (auto id = 0u; id < (unsigned) threadCount; ++id)
        {
            auto expected = expectedNestedSwitch(id);
            mismatches += totals[(int) id] != expected.total;
            mismatches += counters[(int) id] != expected.counter;
        }

        check(mismatches == 0, readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.totals, threadCount, sentinel)
        .output(kernel.counters, threadCount, sentinel)
        .agreeing()
        .run(threadCount, verify);
};

auto tDiscard = test("ControlFlowDevice/discardedColumnsAreNotWritten") = []
{
    if (!canTest())
        return;

    auto program = ColumnDiscardShader {};
    prepare(program, false);

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    auto pixels = drawAndRead(false, draw);

    for (auto i = 0; i < pixelCount; ++i)
    {
        auto column = i % targetSize;
        auto dropped = column == 0 || column == 3;
        check(near(pixels[i], dropped ? -1.f : 1.f));
    }
};

auto tDiscardDepth = test("ControlFlowDevice/aDiscardedFragmentWritesNoDepth") = []
{
    if (!canTest())
        return;

    auto nearer = ColumnDiscardShader {};
    prepare(nearer, true);

    auto farther = FartherQuadShader {};
    prepare(farther, true);

    auto draw = [&](RenderPass& pass)
    {
        pass.draw(nearer);
        pass.draw(farther);
    };

    auto pixels = drawAndRead(true, draw);

    for (auto i = 0; i < pixelCount; ++i)
    {
        auto column = i % targetSize;
        auto dropped = column == 0 || column == 3;
        check(near(pixels[i], dropped ? 2.f : 1.f));
    }
};

auto tFragmentReturn =
    test("ControlFlowDevice/anEarlyFragmentReturnWritesItsChosenColour") = []
{
    if (!canTest())
        return;

    auto program = EarlyFragmentShader {};
    prepare(program, false);

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    check(isSplitAt(drawAndRead(false, draw), targetSize / 2, 0.75f, 0.5f));
};

auto tVertexSwitch = test("ControlFlowDevice/aVertexSwitchPlacesTheQuad") = []
{
    if (!canTest())
        return;

    auto program = VertexSwitchShader {};
    prepare(program, false);

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    check(isSplitAt(drawAndRead(false, draw), targetSize / 2, 1.f, -1.f));
};

auto tVertexReturn =
    test("ControlFlowDevice/anEarlyVertexReturnSkipsWhatFollows") = []
{
    if (!canTest())
        return;

    auto program = VertexReturnShader {};
    program.setVertices(twoQuads);
    program.prepare(describeFloatPipeline(false));

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    check(isSplitAt(drawAndRead(false, draw), targetSize / 2, 1.f, 1.f));
};
} // namespace ControlFlowDeviceTests
