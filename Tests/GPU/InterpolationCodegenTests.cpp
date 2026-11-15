#include "CodegenCommon.h"

#include <eacp/GPU/Codegen/ShaderDialect.h>

#include <initializer_list>
#include <string>

// WGSL's @interpolate(type, sampling) on a varying: perspective, linear or
// flat, at the centre, the centroid or each sample, and first or either for a
// flat one. Each combination is checked as text in all three dialects, and the
// GLSL is compiled wherever eacp-spirv is built. What the rasterizer does with
// each is pinned on a device by InterpolationDeviceTests.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::ShaderEmission;

namespace InterpolationCodegenTests
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

constexpr auto centroid = InterpolationSampling::Centroid;
constexpr auto perSample = InterpolationSampling::Sample;
constexpr auto either = InterpolationSampling::Either;

// Every type and sampling WGSL allows, beside an unasked varying of each
// family.
struct EveryQualifierShader
{
    EveryQualifierShader()
    {
        auto position = builder.vertexInput<Float2>();
        auto shade = builder.vertexInput<Float4>();
        builder.position(float4(position, 0.f, 1.f));

        auto unasked = builder.varying(shade.x());
        auto perspective = builder.varying(shade.y(), Interpolation::perspective());
        auto perspectiveCentroid =
            builder.varying(position, Interpolation::perspective(centroid));
        auto perspectiveSample =
            builder.varying(shade.xyz(), Interpolation::perspective(perSample));
        auto linear = builder.varying(shade, Interpolation::linear());
        auto linearCentroid =
            builder.varying(position, Interpolation::linear(centroid));
        auto linearSample =
            builder.varying(shade.zw(), Interpolation::linear(perSample));
        auto flatFirst = builder.varying(shade.w(), Interpolation::flat());
        auto flatEither = builder.varying(shade, Interpolation::flat(either));
        auto vertex = builder.varying(builder.vertexId());
        auto signedVertex =
            builder.varying(toInt(builder.vertexId()), Interpolation::flat(either));

        auto sum = unasked + perspective + flatFirst + toFloat(vertex)
                   + toFloat(signedVertex);

        builder.fragment(float4(perspectiveCentroid + linearCentroid, linearSample)
                         + float4(perspectiveSample, sum) + linear + flatEither);
    }

    ShaderBuilder builder;
};

const auto everyQualifierMetal = std::string {R"eacp(#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 a0 [[attribute(0)]];
    float4 a1 [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float v0;
    float v1;
    float2 v2 [[centroid_perspective]];
    float3 v3 [[sample_perspective]];
    float4 v4 [[center_no_perspective]];
    float2 v5 [[centroid_no_perspective]];
    float2 v6 [[sample_no_perspective]];
    float v7 [[flat]];
    float4 v8 [[flat]];
    uint v9 [[flat]];
    int v10 [[flat]];
};

vertex VertexOut vertexMain(VertexIn input [[stage_in]], uint vertexId [[vertex_id]])
{
    VertexOut output;
    output.position = float4(input.a0, 0.0, 1.0);
    output.v0 = (input.a1).x;
    output.v1 = (input.a1).y;
    output.v2 = input.a0;
    output.v3 = (input.a1).xyz;
    output.v4 = input.a1;
    output.v5 = input.a0;
    output.v6 = (input.a1).zw;
    output.v7 = (input.a1).w;
    output.v8 = input.a1;
    output.v9 = vertexId;
    output.v10 = int(vertexId);
    return output;
}

fragment float4 fragmentMain(VertexOut input [[stage_in]])
{
    return (((float4((input.v2 + input.v5), input.v6) + float4(input.v3, ((((input.v0 + input.v1) + input.v7) + float(input.v9)) + float(input.v10)))) + input.v4) + input.v8);
}
)eacp"};

const auto everyQualifierHlsl = std::string {R"eacp(struct VertexIn
{
    float2 a0 : TEXCOORD0;
    float4 a1 : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float v0 : TEXCOORD0;
    float v1 : TEXCOORD1;
    centroid float2 v2 : TEXCOORD2;
    sample float3 v3 : TEXCOORD3;
    noperspective float4 v4 : TEXCOORD4;
    noperspective centroid float2 v5 : TEXCOORD5;
    noperspective sample float2 v6 : TEXCOORD6;
    nointerpolation float v7 : TEXCOORD7;
    nointerpolation float4 v8 : TEXCOORD8;
    nointerpolation uint v9 : TEXCOORD9;
    nointerpolation int v10 : TEXCOORD10;
};

VertexOut vertexMain(VertexIn input, uint vertexId : SV_VertexID)
{
    VertexOut output;
    output.position = float4(input.a0, 0.0, 1.0);
    output.v0 = (input.a1).x;
    output.v1 = (input.a1).y;
    output.v2 = input.a0;
    output.v3 = (input.a1).xyz;
    output.v4 = input.a1;
    output.v5 = input.a0;
    output.v6 = (input.a1).zw;
    output.v7 = (input.a1).w;
    output.v8 = input.a1;
    output.v9 = vertexId;
    output.v10 = int(vertexId);
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return (((float4((input.v2 + input.v5), input.v6) + float4(input.v3, ((((input.v0 + input.v1) + input.v7) + float(input.v9)) + float(input.v10)))) + input.v4) + input.v8);
}
)eacp"};

const auto everyQualifierGlsl = std::string {R"eacp(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec4 attr1;
layout(location = 0) out float vary0;
layout(location = 1) out float vary1;
layout(location = 2) centroid out vec2 vary2;
layout(location = 3) sample out vec3 vary3;
layout(location = 4) noperspective out vec4 vary4;
layout(location = 5) noperspective centroid out vec2 vary5;
layout(location = 6) noperspective sample out vec2 vary6;
layout(location = 7) flat out float vary7;
layout(location = 8) flat out vec4 vary8;
layout(location = 9) flat out uint vary9;
layout(location = 10) flat out int vary10;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
    vary0 = (attr1).x;
    vary1 = (attr1).y;
    vary2 = attr0;
    vary3 = (attr1).xyz;
    vary4 = attr1;
    vary5 = attr0;
    vary6 = (attr1).zw;
    vary7 = (attr1).w;
    vary8 = attr1;
    vary9 = uint(gl_VertexIndex);
    vary10 = int(uint(gl_VertexIndex));
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in float vary0;
layout(location = 1) in float vary1;
layout(location = 2) centroid in vec2 vary2;
layout(location = 3) sample in vec3 vary3;
layout(location = 4) noperspective in vec4 vary4;
layout(location = 5) noperspective centroid in vec2 vary5;
layout(location = 6) noperspective sample in vec2 vary6;
layout(location = 7) flat in float vary7;
layout(location = 8) flat in vec4 vary8;
layout(location = 9) flat in uint vary9;
layout(location = 10) flat in int vary10;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = (((vec4((vary2 + vary5), vary6) + vec4(vary3, ((((vary0 + vary1) + vary7) + float(vary9)) + float(vary10)))) + vary4) + vary8);
}
#endif
)eacp"};

// A position and one varying, interpolated as asked or left unasked.
ShaderGraph makeOneVarying(const Interpolation* interpolation)
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    builder.position(float4(position, 0.f, 1.f));

    auto carried = interpolation != nullptr
                       ? builder.varying(position, *interpolation)
                       : builder.varying(position);

    builder.fragment(float4(carried, 0.f, 1.f));
    return builder.graph();
}
} // namespace

static_assert(defaultInterpolation(ValueType::Float3)
              == Interpolation::perspective());
static_assert(defaultInterpolation(ValueType::Int2) == Interpolation::flat());
static_assert(defaultInterpolation(ValueType::UInt).sampling
              == InterpolationSampling::First);

static_assert(isValidInterpolation(ValueType::Float,
                                   Interpolation::linear(centroid)));
static_assert(isValidInterpolation(ValueType::Float4, Interpolation::flat(either)));
static_assert(isValidInterpolation(ValueType::UInt, Interpolation::flat(either)));
static_assert(!isValidInterpolation(ValueType::Int, Interpolation::perspective()));
static_assert(!isValidInterpolation(ValueType::UInt3, Interpolation::linear()));
static_assert(!isValidInterpolation(ValueType::Float,
                                    {InterpolationType::Flat,
                                     InterpolationSampling::Center}));
static_assert(!isValidInterpolation(ValueType::Float,
                                    {InterpolationType::Flat,
                                     InterpolationSampling::Sample}));
static_assert(!isValidInterpolation(ValueType::Float,
                                    {InterpolationType::Perspective,
                                     InterpolationSampling::First}));
static_assert(!isValidInterpolation(ValueType::Float,
                                    {InterpolationType::Linear,
                                     InterpolationSampling::Either}));

static_assert(getMetalInterpolation(Interpolation::perspective()).empty());
static_assert(getMetalInterpolation(Interpolation::perspective(centroid))
              == " [[centroid_perspective]]");
static_assert(getMetalInterpolation(Interpolation::perspective(perSample))
              == " [[sample_perspective]]");
static_assert(getMetalInterpolation(Interpolation::linear())
              == " [[center_no_perspective]]");
static_assert(getMetalInterpolation(Interpolation::linear(centroid))
              == " [[centroid_no_perspective]]");
static_assert(getMetalInterpolation(Interpolation::linear(perSample))
              == " [[sample_no_perspective]]");
static_assert(getMetalInterpolation(Interpolation::flat()) == " [[flat]]");
static_assert(getMetalInterpolation(Interpolation::flat(either)) == " [[flat]]");

auto tEveryQualifier = test("InterpolationCodegen/everyQualifierInEachDialect") = []
{
    auto shader = EveryQualifierShader {};
    auto sources = Sources {shader.builder.graph()};

    check(sources.metal == everyQualifierMetal, sources.metal);
    check(sources.hlsl == everyQualifierHlsl, sources.hlsl);
    check(sources.glsl == everyQualifierGlsl, sources.glsl);

    expectGlslCompiles(shader.builder.graph());
};

auto tRecorded = test("InterpolationCodegen/theGraphRecordsWhatWasAsked") = []
{
    auto shader = EveryQualifierShader {};
    const auto& varyings = shader.builder.graph().varyings();

    check(varyings.size() == 11);
    check(varyings[0].interpolation == Interpolation::perspective());
    check(varyings[1].interpolation == Interpolation::perspective());
    check(varyings[2].interpolation == Interpolation::perspective(centroid));
    check(varyings[3].interpolation == Interpolation::perspective(perSample));
    check(varyings[4].interpolation == Interpolation::linear());
    check(varyings[5].interpolation == Interpolation::linear(centroid));
    check(varyings[6].interpolation == Interpolation::linear(perSample));
    check(varyings[7].interpolation == Interpolation::flat());
    check(varyings[8].interpolation == Interpolation::flat(either));
    check(varyings[9].interpolation == Interpolation::flat());
    check(varyings[10].interpolation == Interpolation::flat(either));
};

// Perspective at the centre is every dialect's default, so asking for it
// prints what leaving it unasked does.
auto tPerspectiveIsUnasked =
    test("InterpolationCodegen/askingForPerspectivePrintsNothing") = []
{
    const auto perspective = Interpolation::perspective();
    const auto unasked = Sources {makeOneVarying(nullptr)};
    const auto asked = Sources {makeOneVarying(&perspective)};

    check(asked.metal == unasked.metal);
    check(asked.hlsl == unasked.hlsl);
    check(asked.glsl == unasked.glsl);

    check(contains(unasked.metal, "    float2 v0;\n"));
    check(contains(unasked.hlsl, "    float2 v0 : TEXCOORD0;\n"));
    check(contains(unasked.glsl, "layout(location = 0) out vec2 vary0;\n"));
    check(contains(unasked.glsl, "layout(location = 0) in vec2 vary0;\n"));
};

// The qualifier sits on both sides of the GLSL interface and once on the one
// struct Metal and HLSL share between the stages.
auto tEachSpelling = test("InterpolationCodegen/eachQualifierAlone") = []
{
    struct Case
    {
        Interpolation interpolation;
        std::string_view metal;
        std::string_view hlsl;
        std::string_view glsl;
    };

    const Case cases[] = {
        {Interpolation::perspective(centroid),
         "    float2 v0 [[centroid_perspective]];\n",
         "    centroid float2 v0 : TEXCOORD0;\n",
         "centroid "},
        {Interpolation::perspective(perSample),
         "    float2 v0 [[sample_perspective]];\n",
         "    sample float2 v0 : TEXCOORD0;\n",
         "sample "},
        {Interpolation::linear(),
         "    float2 v0 [[center_no_perspective]];\n",
         "    noperspective float2 v0 : TEXCOORD0;\n",
         "noperspective "},
        {Interpolation::linear(centroid),
         "    float2 v0 [[centroid_no_perspective]];\n",
         "    noperspective centroid float2 v0 : TEXCOORD0;\n",
         "noperspective centroid "},
        {Interpolation::linear(perSample),
         "    float2 v0 [[sample_no_perspective]];\n",
         "    noperspective sample float2 v0 : TEXCOORD0;\n",
         "noperspective sample "},
        {Interpolation::flat(),
         "    float2 v0 [[flat]];\n",
         "    nointerpolation float2 v0 : TEXCOORD0;\n",
         "flat "},
        {Interpolation::flat(either),
         "    float2 v0 [[flat]];\n",
         "    nointerpolation float2 v0 : TEXCOORD0;\n",
         "flat "},
    };

    for (const auto& each: cases)
    {
        const auto graph = makeOneVarying(&each.interpolation);
        const auto sources = Sources {graph};
        const auto qualifier = std::string {each.glsl};

        check(contains(sources.metal, each.metal), sources.metal);
        check(contains(sources.hlsl, each.hlsl), sources.hlsl);
        check(contains(sources.glsl,
                       "layout(location = 0) " + qualifier + "out vec2 vary0;\n"),
              sources.glsl);
        check(contains(sources.glsl,
                       "layout(location = 0) " + qualifier + "in vec2 vary0;\n"),
              sources.glsl);

        expectGlslCompiles(graph);
    }
};

// An attribute the fragment stage reads directly is carried as a varying the
// author never named, after the named ones, at the default for its type.
auto tCarriedAttribute =
    test("InterpolationCodegen/aCarriedAttributeTakesTheDefault") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto weight = builder.vertexInput<Float>();
    builder.position(float4(position, 0.f, 1.f));

    auto carried = builder.varying(position, Interpolation::linear(centroid));
    builder.fragment(float4(carried, weight, 1.f));

    const auto sources = Sources {builder.graph()};

    check(contains(sources.metal, "    float2 v0 [[centroid_no_perspective]];\n"));
    check(contains(sources.metal, "    float v1;\n"));
    check(contains(sources.hlsl, "    float v1 : TEXCOORD1;\n"));
    check(contains(sources.glsl, "layout(location = 1) in float vary1;\n"));

    expectGlslCompiles(builder.graph());
};

auto tSampleRate =
    test("InterpolationCodegen/onlySampleAndSampleIndexRunPerSample") = []
{
    const auto centroidLinear = Interpolation::linear(centroid);
    const auto sampleLinear = Interpolation::linear(perSample);
    const auto samplePerspective = Interpolation::perspective(perSample);

    check(!makeOneVarying(nullptr).usesSampleRateShading());
    check(!makeOneVarying(&centroidLinear).usesSampleRateShading());
    check(makeOneVarying(&sampleLinear).usesSampleRateShading());
    check(makeOneVarying(&samplePerspective).usesSampleRateShading());

    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    builder.position(float4(position, 0.f, 1.f));
    builder.fragment(float4(toFloat(builder.sampleIndex()), 0.f, 0.f, 1.f));

    check(builder.graph().usesSampleRateShading());
};
} // namespace InterpolationCodegenTests
