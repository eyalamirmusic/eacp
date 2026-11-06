#include "CodegenCommon.h"

#include <climits>
#include <initializer_list>
#include <source_location>
#include <string>

// WGSL's switch, early return, discard, break if and the for loop as the three
// dialects spell them, in the fragment function, the vertex function and a
// kernel. A switch prints one braced clause per case, ended by a break; a
// breakLoop or continueLoop inside a clause is carried out of the switch in a
// jump flag, since a C-family break there only ends the clause and HLSL
// refuses a continue outright. A loop with a continuing block runs it at the
// top of every pass but the first, so a continue runs it too. An early return
// in a render function returns that function's outputs as they evaluate where
// it stands. discardBelow prints byte for byte what it did before discard()
// existed. The GLSL is compiled wherever eacp-spirv is built; what the numbers
// are is pinned by CpuComputeTests' ControlFlow/ and on a device by
// ControlFlowDevice/.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace ControlFlowCodegenTests
{
namespace
{
struct Sources
{
    explicit Sources(const ShaderGraph& graph)
        : metal(emitMetal(graph))
        , hlsl(emitHlsl(graph))
        , glsl(emitGlsl(graph))
    {
    }

    std::string metal;
    std::string hlsl;
    std::string glsl;
};

// The vertex function's text and the fragment function's, split where the
// fragment one starts.
struct Stages
{
    explicit Stages(const std::string& source)
    {
        const auto glsl = contains(source, "#ifdef EACP_VERTEX");
        const auto vertexAt =
            source.find(glsl ? "#ifdef EACP_VERTEX" : "vertexMain(");
        const auto fragmentAt =
            source.find(glsl ? "#ifdef EACP_FRAGMENT" : "fragmentMain(");

        vertex = source.substr(vertexAt, fragmentAt - vertexAt);
        fragment = source.substr(fragmentAt);
    }

    std::string vertex;
    std::string fragment;
};

void checkAll(const std::string& source,
              std::initializer_list<const char*> expected,
              const std::source_location& location = std::source_location::current())
{
    for (const auto* text: expected)
        check(contains(source, text), source, location);
}

Float4 opaque(ShaderBuilder& builder, float red, float green, float blue)
{
    return float4(builder.constant(red), green, blue, 1.f);
}

// A colour picked by a uniform mode: 0 red, 1 or 2 green, anything else blue.
struct ModeShader
{
    ModeShader()
    {
        auto position = builder.vertexInput<Float2>();
        auto mode = builder.uniform<Int>();
        builder.position(float4(position, 0.f, 1.f));

        auto colour = builder.var(opaque(builder, 0.f, 0.f, 0.f));
        auto red = [&] { colour = opaque(builder, 1.f, 0.f, 0.f); };
        auto green = [&] { colour = opaque(builder, 0.f, 1.f, 0.f); };
        auto blue = [&] { colour = opaque(builder, 0.f, 0.f, 1.f); };

        builder.switchOn(
            mode, caseOf(0, red), caseOf({1, 2}, green), defaultCase(blue));
        builder.fragment(colour.get());
    }

    ShaderBuilder builder;
};
} // namespace

auto tSwitchClauses =
    test("ControlFlowCodegen/aSwitchPrintsOneBracedClausePerCase") = []
{
    auto shader = ModeShader {};
    const auto& graph = shader.builder.graph();
    auto sources = Sources {graph};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        auto fragment = Stages {*source}.fragment;
        checkAll(fragment,
                 {"    switch (uniforms.u0)\n"
                  "    {\n"
                  "        case 0:\n"
                  "        {\n"
                  "            v0 = ",
                  "            break;\n"
                  "        }\n"
                  "        case 1:\n"
                  "        case 2:\n"
                  "        {\n",
                  "        default:\n"
                  "        {\n"});
        check(occurrences(fragment, "break;") == 3, fragment);
        check(!contains(fragment, "jump"), fragment);
    }

    check(fragmentReadsUniforms(graph));
    check(!vertexReadsUniforms(graph));
    expectGlslCompiles(sources.glsl);
};

auto tSwitchLabels =
    test("ControlFlowCodegen/labelsAreSpelledInTheSelectorsType") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.uintOutputBuffer();
    auto id = builder.threadId();
    auto value = builder.var(0u);

    auto one = [&] { value = 1u; };
    auto two = [&] { value = 2u; };
    builder.switchOn(id, caseOf({0, 4000000000LL}, one), defaultCase(two));

    auto signedValue = toInt(id) - 2;
    builder.switchOn(signedValue, caseOf({INT_MIN, -1}, one), caseOf(INT_MAX, two));
    builder.write(output, id, value.get());

    auto sources = Sources {builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        checkAll(*source,
                 {"case 0u:",
                  "case 4000000000u:",
                  "case (-2147483647 - 1):",
                  "case -1:",
                  "case 2147483647:"});
        check(!contains(*source, "case 0:"), *source);
    }

    expectGlslCompiles(sources.glsl, true);
};

namespace
{
// A counter walked to eight whose step a switch picks: a multiple of three
// continues the loop from inside the switch, a remainder of one adds the
// counter, and any other breaks the loop once the total passes ten.
struct JumpingKernel
{
    JumpingKernel()
    {
        auto output = builder.uintOutputBuffer();
        auto id = builder.threadId();
        auto counter = builder.var(id);
        auto total = builder.var(0u);

        auto skip = [&] { builder.continueLoop(); };
        auto add = [&] { total += counter.get(); };
        auto stop = [&] { builder.breakLoop(); };
        auto maybeStop = [&] { builder.ifThen(total.get() > 10u, stop); };

        auto step = [&]
        {
            counter += 1u;
            builder.switchOn(counter.get() % 3u,
                             caseOf(0, skip),
                             caseOf(1, add),
                             defaultCase(maybeStop));
            total += 100u;
        };

        builder.loop(counter.get() < 8u, step);
        builder.write(output, id, total.get());
    }

    ShaderBuilder builder;
};
} // namespace

auto tJumpOutOfSwitch =
    test("ControlFlowCodegen/aLoopJumpInsideASwitchLeavesTheLoop") = []
{
    auto kernel = JumpingKernel {};
    auto sources = Sources {kernel.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        checkAll(*source,
                 {"        int jump0 = 0;\n"
                  "        switch ((v0 % 3u))\n",
                  "            case 0u:\n"
                  "            {\n"
                  "                jump0 = 2;\n"
                  "                break;\n"
                  "            }\n",
                  "                    jump0 = 1;\n"
                  "                    break;\n",
                  "        if (jump0 == 1)\n"
                  "        {\n"
                  "            break;\n"
                  "        }\n"
                  "        if (jump0 == 2)\n"
                  "        {\n"
                  "            continue;\n"
                  "        }\n"});
        check(occurrences(*source, "continue;") == 1, *source);
    }

    expectGlslCompiles(sources.glsl, true);
};

auto tNestedSwitch =
    test("ControlFlowCodegen/aJumpOutOfANestedSwitchPassesThroughTheOuter") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.uintOutputBuffer();
    auto id = builder.threadId();
    auto counter = builder.var(0u);

    auto stop = [&] { builder.breakLoop(); };
    auto inner = [&] { builder.switchOn(id % 2u, caseOf(1, stop)); };
    auto step = [&]
    {
        counter += 1u;
        builder.switchOn(counter.get(), caseOf(3, inner));
    };

    builder.loop(counter.get() < 8u, step);
    builder.write(output, id, counter.get());

    auto sources = Sources {builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
        checkAll(*source,
                 {"int jump0 = 0;",
                  "int jump1 = 0;",
                  "jump1 = 1;",
                  "                if (jump1 == 1)\n"
                  "                {\n"
                  "                    jump0 = 1;\n"
                  "                    break;\n"
                  "                }\n",
                  "        if (jump0 == 1)\n"
                  "        {\n"
                  "            break;\n"
                  "        }\n"});

    expectGlslCompiles(sources.glsl, true);
};

auto tVertexSwitch = test("ControlFlowCodegen/aVertexSwitchIsEmittedThere") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();

    auto place = [&]
    {
        auto x = builder.var(position.x());
        auto left = [&] { x = builder.constant(-1.f); };
        auto right = [&] { x = builder.constant(0.f); };
        builder.switchOn(
            builder.vertexId(), caseOf({0, 2, 5}, left), caseOf({1, 3, 4}, right));
        builder.position(float4(x.get(), position.y(), 0.f, 1.f));
    };

    builder.vertexStage(place);
    builder.fragment(opaque(builder, 1.f, 1.f, 1.f));

    auto sources = Sources {builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        auto stages = Stages {*source};
        checkAll(stages.vertex,
                 {"switch (", "case 0u:\n", "case 5u:\n", "case 4u:\n"});
        check(!contains(stages.fragment, "switch"), *source);
    }

    expectGlslCompiles(sources.glsl);
};

auto tKernelReturn = test("ControlFlowCodegen/aKernelReturnsBehindItsGuard") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.outputBuffer();
    auto id = builder.threadId();

    auto leave = [&] { builder.returnEarly(); };
    builder.ifThen(id % 2u == 0u, leave);
    builder.write(output, id, builder.constant(1.f));

    auto sources = Sources {builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
        checkAll(*source,
                 {"    if (gid >= uniforms.count)\n        return;\n",
                  "    if (((gid % 2u) == 0u))\n"
                  "    {\n"
                  "        return;\n"
                  "    }\n"});

    expectGlslCompiles(sources.glsl, true);
};

namespace
{
// Red where the varying is positive, returned from inside the branch; blue
// otherwise, written after it.
struct EarlyFragment
{
    explicit EarlyFragment(bool withDepth, bool withAlphaTest)
    {
        auto position = builder.vertexInput<Float2>();
        builder.position(float4(position, 0.f, 1.f));
        auto carried = builder.varying(position);

        auto shade = builder.var(opaque(builder, 0.f, 0.f, 0.f));
        auto leave = [&]
        {
            shade = opaque(builder, 1.f, 0.f, 0.f);
            builder.returnEarly();
        };

        builder.ifThen(carried.x() > 0.f, leave);
        shade = opaque(builder, 0.f, 0.f, 1.f);
        builder.fragment(shade.get());

        if (withDepth)
            builder.fragmentDepth(builder.constant(0.25f));

        if (withAlphaTest)
            builder.discardBelow(carried.y(), 0.5f);
    }

    ShaderBuilder builder;
};
} // namespace

auto tFragmentReturn =
    test("ControlFlowCodegen/aFragmentReturnsItsColourWhereItStands") = []
{
    auto shader = EarlyFragment {false, false};
    auto sources = Sources {shader.builder.graph()};

    auto metal = Stages {sources.metal}.fragment;
    checkAll(metal,
             {"    if (((input.v0).x > 0.0))\n"
              "    {\n"
              "        v0 = float4(1.0, 0.0, 0.0, 1.0);\n"
              "        return v0;\n"
              "    }\n"
              "    v0 = float4(0.0, 0.0, 1.0, 1.0);\n"
              "    return v0;\n"});

    auto hlsl = Stages {sources.hlsl}.fragment;
    checkAll(hlsl, {"        return v0;\n    }\n", "    return v0;\n}"});

    auto glsl = Stages {sources.glsl}.fragment;
    checkAll(glsl,
             {"        fragColor = v0;\n"
              "        return;\n"
              "    }\n",
              "    fragColor = v0;\n}"});

    expectGlslCompiles(sources.glsl);
};

auto tFragmentReturnStruct =
    test("ControlFlowCodegen/anEarlyReturnFillsTheWholeFragmentOut") = []
{
    auto shader = EarlyFragment {true, true};
    auto sources = Sources {shader.builder.graph()};

    auto metal = Stages {sources.metal}.fragment;
    checkAll(metal,
             {"        if ((input.v0).y < 0.5)\n"
              "            discard_fragment();\n"
              "        {\n"
              "            FragmentOut output;\n"
              "            output.color = v0;\n"
              "            output.depth = 0.25;\n"
              "            return output;\n"
              "        }\n"
              "    }\n"});
    check(occurrences(metal, "discard_fragment();") == 2, metal);

    auto hlsl = Stages {sources.hlsl}.fragment;
    checkAll(hlsl,
             {"            discard;\n"
              "        {\n"
              "            FragmentOut output;\n"});

    auto glsl = Stages {sources.glsl}.fragment;
    checkAll(glsl,
             {"            discard;\n"
              "        fragColor = v0;\n"
              "        gl_FragDepth = 0.25;\n"
              "        return;\n"});

    expectGlslCompiles(sources.glsl);
};

auto tVertexReturn =
    test("ControlFlowCodegen/aVertexReturnsItsOutputsWhereItStands") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto carried = Float {};

    auto place = [&]
    {
        auto at = builder.var(float4(position, 0.f, 1.f));
        auto height = builder.var(position.y());
        auto collapse = [&]
        {
            at = float4(builder.constant(0.f), 0.f, 0.f, 1.f);
            height = builder.constant(2.f);
            builder.returnEarly();
        };

        builder.ifThen(builder.vertexId() > 5u, collapse);
        builder.position(at.get());
        carried = builder.varying(height.get());
    };

    builder.vertexStage(place);
    builder.fragment(float4(carried, carried, carried, 1.f));

    auto sources = Sources {builder.graph()};

    auto metal = Stages {sources.metal}.vertex;
    checkAll(metal,
             {"        v1 = 2.0;\n"
              "        {\n"
              "            VertexOut output;\n"
              "            output.position = v0;\n"
              "            output.v0 = v1;\n"
              "            return output;\n"
              "        }\n"
              "    }\n"
              "    VertexOut output;\n"});

    auto hlsl = Stages {sources.hlsl}.vertex;
    checkAll(hlsl, {"            return output;\n        }\n"});

    auto glsl = Stages {sources.glsl}.vertex;
    checkAll(glsl,
             {"        gl_Position = v0;\n"
              "        vary0 = v1;\n"
              "        return;\n"
              "    }\n"});

    expectGlslCompiles(sources.glsl);
};

auto tDiscard =
    test("ControlFlowCodegen/discardIsAStatementTakenAnyNumberOfTimes") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    builder.position(float4(position, 0.f, 1.f));
    auto carried = builder.varying(position);

    auto drop = [&] { builder.discard(); };
    builder.ifThen(carried.x() > 0.5f, drop);
    builder.ifThen(carried.y() > 0.5f, drop);
    builder.fragment(opaque(builder, 1.f, 1.f, 1.f));

    auto sources = Sources {builder.graph()};

    auto metal = Stages {sources.metal}.fragment;
    check(occurrences(metal, "        discard_fragment();\n") == 2, metal);

    for (const auto* source: {&sources.hlsl, &sources.glsl})
    {
        auto fragment = Stages {*source}.fragment;
        check(occurrences(fragment, "        discard;\n") == 2, fragment);
    }

    expectGlslCompiles(sources.glsl);
};

namespace
{
// A loop over [0, 10) that skips the even counters with a continue, whose
// update and break-if sit in the continuing block.
template <typename Record>
std::string continuingLoopOf(Record&& record)
{
    auto builder = ShaderBuilder {};
    auto output = builder.uintOutputBuffer();
    auto id = builder.threadId();
    record(builder);
    builder.write(output, id, 0u);
    return emitMetal(builder.graph());
}
} // namespace

auto tContinuing =
    test("ControlFlowCodegen/aContinuingBlockRunsAtTheTopOfEveryPass") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.uintOutputBuffer();
    auto id = builder.threadId();
    auto counter = builder.var(0u);
    auto total = builder.var(0u);

    auto skip = [&] { builder.continueLoop(); };
    auto body = [&]
    {
        builder.ifThen(counter.get() % 2u == 0u, skip);
        total += counter.get();
    };

    auto update = [&]
    {
        counter += 1u;
        builder.breakLoopIf(total.get() > id);
    };

    builder.loop(counter.get() < 10u, body, update);
    builder.write(output, id, total.get());

    auto sources = Sources {builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
        checkAll(*source,
                 {"    bool firstPass0 = true;\n"
                  "    while (true)\n"
                  "    {\n"
                  "        if (!firstPass0)\n"
                  "        {\n"
                  "            v0 = (v0 + 1u);\n"
                  "            if ((v1 > gid))\n"
                  "            {\n"
                  "                break;\n"
                  "            }\n"
                  "        }\n"
                  "        firstPass0 = false;\n"
                  "        if (!(v0 < 10u))\n"
                  "        {\n"
                  "            break;\n"
                  "        }\n"
                  "        if (((v0 % 2u) == 0u))\n"
                  "        {\n"
                  "            continue;\n"
                  "        }\n"
                  "        v1 = (v1 + v0);\n"
                  "    }\n"});

    expectGlslCompiles(sources.glsl, true);
};

auto tForLoop =
    test("ControlFlowCodegen/aForLoopIsALoopWithItsUpdateContinuing") = []
{
    auto recordFor = [](ShaderBuilder& builder)
    {
        auto counter = builder.var(0u);
        auto total = builder.var(0u);
        auto update = [&] { counter += 1u; };
        auto body = [&] { total += counter.get(); };
        builder.forLoop(counter.get() < 4u, update, body);
    };

    auto recordLoop = [](ShaderBuilder& builder)
    {
        auto counter = builder.var(0u);
        auto total = builder.var(0u);
        auto update = [&] { counter += 1u; };
        auto body = [&] { total += counter.get(); };
        builder.loop(counter.get() < 4u, body, update);
    };

    auto viaFor = continuingLoopOf(recordFor);
    check(viaFor == continuingLoopOf(recordLoop), viaFor);
    check(contains(viaFor, "bool firstPass0 = true;"), viaFor);
};

auto tVertexContinuing =
    test("ControlFlowCodegen/theVertexStageTakesEveryNewStatement") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();

    auto place = [&]
    {
        auto height = builder.var(0.f);
        auto steps = builder.var(0u);
        auto climb = [&] { height += 0.25f; };
        auto update = [&]
        {
            steps += 1u;
            builder.breakLoopIf(height.get() > 1.f);
        };

        builder.loop(steps.get() < 8u, climb, update);

        auto flatten = [&]
        {
            height = builder.constant(0.f);
            builder.returnEarly();
        };

        builder.switchOn(builder.instanceId(), caseOf(3, flatten));
        builder.position(float4(position, height.get(), 1.f));
    };

    builder.vertexStage(place);
    builder.fragment(opaque(builder, 1.f, 1.f, 1.f));

    auto sources = Sources {builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        auto stages = Stages {*source};
        checkAll(stages.vertex, {"firstPass0", "switch (", "case 3u:"});
        check(!contains(stages.fragment, "switch"), *source);
    }

    expectGlslCompiles(sources.glsl);
};

namespace
{
// A sprite's alpha test: the colour is the varying, killed below half.
ShaderGraph alphaTestedSprite()
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto tint = builder.uniform<Float4>();
    builder.position(float4(position, 0.f, 1.f));

    auto uv = builder.varying(position * 0.5f + 0.5f);
    builder.fragment(float4(uv, 0.f, 1.f) * tint);
    builder.discardBelow(uv.x() * tint.w(), 0.5f);
    return builder.graph();
}

// Each one byte for byte what the codegen emitted before discard() and
// returnEarly() were added.
const auto alphaTestMetal = std::string {R"eacp(#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 a0 [[attribute(0)]];
};

struct VertexOut
{
    float4 position [[position]];
    float2 v0;
};

struct Uniforms
{
    float4 u0;
};

vertex VertexOut vertexMain(VertexIn input [[stage_in]])
{
    VertexOut output;
    output.position = float4(input.a0, 0.0, 1.0);
    output.v0 = ((input.a0 * 0.5) + 0.5);
    return output;
}

fragment float4 fragmentMain(VertexOut input [[stage_in]],
    constant Uniforms& uniforms [[buffer(16)]])
{
    if (((input.v0).x * (uniforms.u0).w) < 0.5)
        discard_fragment();
    return (float4(input.v0, 0.0, 1.0) * uniforms.u0);
}
)eacp"};
} // namespace

auto tAlphaTestGolden =
    test("ControlFlowCodegen/discardBelowIsByteIdenticalToBefore") = []
{
    auto graph = alphaTestedSprite();
    auto metal = emitMetal(graph);
    check(metal == alphaTestMetal, metal);

    auto hlsl = Stages {emitHlsl(graph)}.fragment;
    check(contains(hlsl,
                   "{\n"
                   "    if (((input.v0).x * (uniforms.u0).w) < 0.5)\n"
                   "        discard;\n"
                   "    return (float4(input.v0, 0.0, 1.0) * uniforms.u0);\n"
                   "}\n"),
          hlsl);

    auto glsl = Stages {emitGlsl(graph)}.fragment;
    check(contains(glsl,
                   "{\n"
                   "    if (((vary0).x * (uniforms.u0).w) < 0.5)\n"
                   "        discard;\n"
                   "    fragColor = (vec4(vary0, 0.0, 1.0) * uniforms.u0);\n"
                   "}\n"),
          glsl);

    expectGlslCompiles(graph);
};
} // namespace ControlFlowCodegenTests
