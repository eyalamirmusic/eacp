#include "CodegenCommon.h"

#include <eacp/Core/Utils/Format.h>

#include <initializer_list>
#include <source_location>
#include <string>

// WGSL's f16 as the three dialects spell it: MSL's half and GLSL's float16_t
// natively, the latter under GL_EXT_shader_explicit_arithmetic_types_float16
// (and GL_EXT_shader_16bit_storage for an f16 buffer), both named only where a
// shader uses one; HLSL holds every f16 in float and wraps each arithmetic
// result in eacpRoundHalf, f32tof16 and back, and an f16 buffer is its words.
// The GLSL is compiled wherever eacp-spirv is built; the numbers are pinned by
// CpuComputeTests' Float16/ and on a device by Float16Device/.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace Float16CodegenTests
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

void checkAll(const std::string& source,
              std::initializer_list<const char*> expected,
              const std::source_location& location = std::source_location::current())
{
    for (const auto* text: expected)
        check(contains(source, text), std::string(text) + "\n" + source, location);
}

void checkNone(
    const std::string& source,
    std::initializer_list<const char*> refused,
    const std::source_location& location = std::source_location::current())
{
    for (const auto* text: refused)
        check(!contains(source, text), std::string(text) + "\n" + source, location);
}

// The smallest kernel with every kind of f16 node: a buffer read, a literal,
// an operator, a built-in, a conversion each way and a buffer write.
struct SmallGraph
{
    SmallGraph()
    {
        auto input = builder.halfInputBuffer();
        auto output = builder.halfOutputBuffer();
        auto id = builder.threadId();

        auto x = input[id];
        auto scaled = x * 0.5f + toHalf(toFloat(id));
        builder.write(output, id, sqrt(scaled));
    }

    ShaderBuilder builder;
};

constexpr auto smallMetal = std::string_view {R"golden(#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    uint count;
};

kernel void computeMain(device const half* buffer0 [[buffer(0)]],
    device half* buffer1 [[buffer(1)]],
    constant Uniforms& uniforms [[buffer(16)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= uniforms.count)
        return;
    buffer1[gid] = sqrt(((buffer0[gid] * 0.5h) + half(float(gid))));
}
)golden"};

constexpr auto smallHlsl = std::string_view {R"golden(float eacpRoundHalf(float x)
{
    return f16tof32(f32tof16(x));
}

struct Uniforms
{
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<uint> buffer0 : register(t0);
RWStructuredBuffer<uint> buffer1 : register(u1);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    InterlockedAnd(buffer1[(gid) >> 1u], ~(0xffffu << (((gid) & 1u) << 4u)));
    InterlockedOr(buffer1[(gid) >> 1u], f32tof16(eacpRoundHalf(sqrt(eacpRoundHalf((eacpRoundHalf((f16tof32(buffer0[(gid) >> 1u] >> (((gid) & 1u) << 4u)) * 0.5)) + eacpRoundHalf(float(float(gid)))))))) << (((gid) & 1u) << 4u));
}
)golden"};

constexpr auto smallGlsl = std::string_view {R"golden(#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_16bit_storage : require

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint count;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    float16_t buffer0[];
};
layout(std430, set = 0, binding = 1) buffer Buffer1
{
    float16_t buffer1[];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    buffer1[gid] = sqrt(((buffer0[gid] * 0.5hf) + float16_t(float(gid))));
}
)golden"};

// Arithmetic, comparisons, selection, swizzles and the conversions, beside a
// Float computation that must stay f32.
struct ArithmeticGraph
{
    ArithmeticGraph()
    {
        auto input = builder.halfInputBuffer();
        auto floats = builder.inputBuffer();
        auto output = builder.halfOutputBuffer();
        auto id = builder.threadId();

        auto a = input.read4(id);
        auto b = half3(a.xy(), 1.5f);
        auto c = a.zyx() / b - 2.f;
        auto mask = a.xyz() < c;
        auto pick = select(any(mask), dot(b, c), -a.w());
        auto converted = toHalf(floats[id] * 3.f) + toHalf(toInt(a.x()));
        auto back = toFloat(pick) + floats[id];

        builder.write(output, id, half4(c, pick + converted));
        builder.write(output, id * 2u, toHalf(back));
        builder.write(output, id + 7u, toHalf(toUInt(c).x()));
    }

    ShaderBuilder builder;
};

// Every f16 built-in WGSL has, on a vector and on a scalar.
struct BuiltinGraph
{
    BuiltinGraph()
    {
        auto input = builder.halfInputBuffer();
        auto output = builder.halfOutputBuffer();
        auto id = builder.threadId();

        auto v = input.read3(id);
        auto s = input[id];
        auto at = 0u;
        auto put = [&](const Half3& value)
        { builder.write(output, id * 64u + at++, value); };

        put(sin(v) + cos(v) + tan(v));
        put(asin(v) + acos(v) + atan(v) + atan2(v, s));
        put(sinh(v) + cosh(v) + tanh(v));
        put(asinh(v) + acosh(v + 1.f) + atanh(v * 0.5f));
        put(exp(v) + exp2(v) + log(abs(v) + 1.f) + log2(abs(v) + 1.f));
        put(sqrt(abs(v)) + rsqrt(abs(v) + 1.f) + pow(abs(v), 2.f));
        put(floor(v) + ceil(v) + round(v) + trunc(v) + fract(v) + sign(v));
        put(min(v, s) + max(v, 0.25f) + clamp(v, -1.f, s));
        put(mix(v, v * 2.f, 0.5f) + step(0.f, v) + smoothstep(0.f, 1.f, v));
        put(fma(v, v, s) + degrees(v) + radians(v) + mod(v, 0.75f));
        put(normalize(v + 1.f) + cross(v, v.yzx()));
        put(reflect(v, v.zxy()) + refract(v, v.zxy(), s)
            + faceforward(v, v.zxy(), v));
        put(half3(length(v), distance(v, v.zxy()), dot(v, v)));
    }

    ShaderBuilder builder;
};

template <int Width>
HalfVectorOf<Width> readVector(const HalfInputBuffer& values, unsigned first)
{
    auto at = [&](unsigned offset) { return values[first + offset]; };

    if constexpr (Width == 2)
        return half2(at(0u), at(1u));
    else if constexpr (Width == 3)
        return half3(at(0u), at(1u), at(2u));
    else
        return half4(at(0u), at(1u), at(2u), at(3u));
}

template <int Columns, int Rows>
HalfMatrixOf<Columns, Rows> readMatrix(const HalfInputBuffer& values)
{
    auto column = [&](int which)
    { return readVector<Rows>(values, (unsigned) (which * Rows)); };

    if constexpr (Columns == 2 && Rows == 2)
        return half2x2(column(0), column(1));
    else if constexpr (Columns == 2 && Rows == 3)
        return half2x3(column(0), column(1));
    else if constexpr (Columns == 2 && Rows == 4)
        return half2x4(column(0), column(1));
    else if constexpr (Columns == 3 && Rows == 2)
        return half3x2(column(0), column(1), column(2));
    else if constexpr (Columns == 3 && Rows == 3)
        return half3x3(column(0), column(1), column(2));
    else if constexpr (Columns == 3 && Rows == 4)
        return half3x4(column(0), column(1), column(2));
    else if constexpr (Columns == 4 && Rows == 2)
        return half4x2(column(0), column(1), column(2), column(3));
    else if constexpr (Columns == 4 && Rows == 3)
        return half4x3(column(0), column(1), column(2), column(3));
    else
        return half4x4(column(0), column(1), column(2), column(3));
}

template <int Width>
void writeVector(ShaderBuilder& builder,
                 const HalfOutputBuffer& output,
                 unsigned& at,
                 const HalfVectorOf<Width>& value)
{
    builder.write(output, builder.unsignedInteger(at), value);
    at += (unsigned) Width;
}

// One f16 matrix shape: built, read a column at a fixed and a dynamic index,
// transposed, multiplied by a vector either side and by its transpose, scaled,
// summed, differenced, held in a var and a private array.
template <int Columns, int Rows>
struct ShapeGraph
{
    ShapeGraph()
    {
        auto values = builder.halfInputBuffer();
        auto output = builder.halfOutputBuffer();
        auto id = builder.threadId();
        auto at = 0u;

        auto m = readMatrix<Columns, Rows>(values);
        auto byColumns = readVector<Columns>(values, 20u);
        auto byRows = readVector<Rows>(values, 24u);
        auto square = m * transpose(m);
        auto sum = (m + m * 2.f - m / 4.f) * byColumns;

        auto held = builder.var(m);
        held = held.get() * values[id];

        auto kept = builder.privateArray<HalfMatrixOf<Columns, Rows>, 2>();
        kept.set(1, m);

        writeVector<Rows>(builder, output, at, m[1]);
        writeVector<Rows>(builder, output, at, m[toInt(id) - 1]);
        writeVector<Rows>(builder, output, at, m * byColumns);
        writeVector<Columns>(builder, output, at, byRows * m);
        writeVector<Rows>(builder, output, at, square[0]);
        writeVector<Columns>(builder, output, at, transpose(m)[id]);
        writeVector<Rows>(builder, output, at, sum);
        writeVector<Rows>(builder, output, at, held.get()[0]);
        writeVector<Rows>(builder, output, at, kept[id % 2u][1]);

        if constexpr (Columns == Rows)
            builder.write(output, id, determinant(m));
    }

    ShaderBuilder builder;
};

struct StatementGraph
{
    StatementGraph()
    {
        auto input = builder.halfInputBuffer();
        auto output = builder.halfOutputBuffer();
        auto id = builder.threadId();

        auto total = builder.var(builder.halfConstant(0.f));
        auto items = builder.privateArray<Half, 4>();
        auto counter = builder.var(0);

        auto body = [&]
        {
            items.set(counter.get(), input[id + toUInt(counter.get())]);
            total += items[counter.get()] * 0.5f;
            counter += 1;
        };
        builder.loop(counter.get() < 4, body);

        items.set(0, 0.125f);
        total = 0.25f;

        auto table =
            builder.array(builder.halfConstant(0.5f), builder.halfConstant(1.5f));
        auto picked = table[toInt(id % 2u)];
        builder.write(
            output, id, total.get() + items[0] + items[toInt(id)] + picked);
    }

    ShaderBuilder builder;
};

// A render shader computing in f16 inside the fragment stage, from a Float
// varying, and a fragment that reads an f16 buffer.
struct FragmentGraph
{
    FragmentGraph()
    {
        auto position = builder.vertexInput<Float2>();
        auto shades = builder.halfInputBuffer();
        auto uv = builder.varying(position * 0.5f + 0.5f);
        builder.position(float4(position, 0.f, 1.f));

        auto tint = toHalf(uv) * shades[0u] + 0.25f;
        builder.fragment(float4(toFloat(tint), 0.f, 1.f));
    }

    ShaderBuilder builder;
};

struct FloatOnlyGraph
{
    FloatOnlyGraph()
    {
        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();
        builder.write(output, id, quantizeToF16(input[id]) + input.readHalf(id));
    }

    ShaderBuilder builder;
};

struct HalfArithmeticOnlyGraph
{
    HalfArithmeticOnlyGraph()
    {
        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();
        builder.write(output, id, toFloat(toHalf(input[id]) * 3.f));
    }

    ShaderBuilder builder;
};

template <typename L, typename R>
concept Adds = requires(const L& a, const R& b) { a + b; };

template <typename L, typename R>
concept Multiplies = requires(const L& a, const R& b) { a * b; };

template <typename L, typename R>
concept Compares = requires(const L& a, const R& b) { a < b; };

template <typename L, typename R>
concept Mins = requires(const L& a, const R& b) { min(a, b); };

template <typename T>
concept Derives = requires(const T& a) { dfdx(a); };

template <typename T>
concept Quantizes = requires(const T& a) { quantizeToF16(a); };

template <typename T>
concept TakesErf = requires(const T& a) { erf(a); };

template <typename... Args>
concept BuildsFloat3 = requires(const Args&... args) { float3(args...); };

template <typename... Args>
concept BuildsHalf3 = requires(const Args&... args) { half3(args...); };
} // namespace

auto tTypeNames = test("Float16Codegen/eachTypeIsSpelledPerDialect") = []
{
    check(std::string(typeName(ValueType::Half)) == "half");
    check(std::string(typeName(ValueType::Half3)) == "half3");
    check(std::string(typeName(ValueType::Half2x3)) == "half2x3");
    check(std::string(glslTypeName(ValueType::Half)) == "float16_t");
    check(std::string(glslTypeName(ValueType::Half4)) == "f16vec4");
    check(std::string(glslTypeName(ValueType::Half3x3)) == "f16mat3");
    check(std::string(glslTypeName(ValueType::Half2x3)) == "f16mat2x3");
    check(std::string(hlslTypeName(ValueType::Half)) == "float");
    check(std::string(hlslTypeName(ValueType::Half2)) == "float2");
    check(std::string(hlslTypeName(ValueType::Half2x3)) == "float3x2");
    check(std::string(hlslTypeName(ValueType::Half4x4)) == "float4x4");

    check(isHalf(ValueType::Half3x4) && !isHalf(ValueType::Float3x4));
    check(byteSize(ValueType::Half3) == 6);
    check(componentCount(ValueType::Half4x3) == 12);
    check(matrixColumnType(ValueType::Half4x3) == ValueType::Half3);
    check(transposedMatrix(ValueType::Half2x4) == ValueType::Half4x2);
    check(halfEquivalent(ValueType::Float2x3) == ValueType::Half2x3);
    check(floatEquivalent(ValueType::Half4) == ValueType::Float4);
    check(floatEquivalent(ValueType::UInt2) == ValueType::UInt2);
};

auto tFamilies = test("Float16Codegen/theTwoFloatFamiliesNeverMix") = []
{
    check(Adds<Half, Half> && Adds<Half3, Half> && Adds<Half, float>);
    check(!Adds<Half, Float> && !Adds<Float3, Half3> && !Adds<Half3, Float>);
    check(Multiplies<Half2x3, Half2> && Multiplies<Half3, Half2x3>);
    check(!Multiplies<Half2x3, Half3> && Multiplies<Half3x3, Half3x3>);
    check(Multiplies<Half2x3, Half3x2> && !Multiplies<Half2x3, Half2x3>);
    check(Multiplies<Half2x3, Half> && Multiplies<Half2x3, float>);
    check(!Multiplies<Half3x3, Float3> && !Multiplies<Float3x3, Half3>);
    check(!Multiplies<Half3x3, Float3x3> && !Multiplies<Half2x3, Float>);
    check(Compares<Half, Half> && Compares<Half, float> && !Compares<Half, Float>);
    check(Compares<Half3, Half3> && !Compares<Half3, Float3>);
    check(Mins<Half3, Half> && !Mins<Half3, Float> && !Mins<Float3, Half>);

    check(Derives<Float> && !Derives<Half3>);
    check(Quantizes<Float2> && !Quantizes<Half2>);
    check(TakesErf<Float> && !TakesErf<Half>);

    check(BuildsFloat3<Float2, float> && !BuildsFloat3<Half2, float>);
    check(BuildsHalf3<Half2, float> && !BuildsHalf3<Float2, float>);
    check(!BuildsHalf3<Half2, Float>);
};

auto tSmall = test("Float16Codegen/aSmallKernelMatchesTheGoldens") = []
{
    auto graph = SmallGraph {};
    auto sources = Sources {graph.builder.graph()};

    check(sources.metal == smallMetal, sources.metal);
    check(sources.hlsl == smallHlsl, sources.hlsl);
    check(sources.glsl == smallGlsl, sources.glsl);
    expectGlslCompiles(sources.glsl, true);
};

auto tArithmetic =
    test("Float16Codegen/operatorsAndConversionsStayInTheirFamily") = []
{
    auto graph = ArithmeticGraph {};
    auto sources = Sources {graph.builder.graph()};

    checkAll(sources.metal,
             {"half4 t1 = half4(*((device const packed_half4*) (buffer0 + t0)));",
              "half3((t1).xy, 1.5h)",
              "- 2.0h",
              "half((t5 * 3.0))",
              "half(int((t1).x))",
              "half((float(t4) + t5))",
              "float t5 = buffer1[gid];"});

    checkAll(sources.hlsl,
             {"float eacpRoundHalf(float x)",
              "float3 eacpRoundHalf(float3 x)",
              "eacpRoundHalf((eacpRoundHalf(((t1).zyx / t2)) - 2.0))",
              "eacpRoundHalf(dot(t2, t3))",
              "eacpRoundHalf(float((t5 * 3.0)))",
              "(-((t1).w))",
              "float t5 = buffer1[gid];"});
    checkNone(sources.hlsl, {"half", "eacpRoundHalf((t5", "eacpRoundHalf(-("});

    checkAll(sources.glsl,
             {"f16vec3 t2 = f16vec3((t1).xy, 1.5hf);",
              "any(lessThan((t1).xyz, t3))",
              "float16_t((t5 * 3.0))",
              "float16_t((uvec3(t3)).x)"});
    expectGlslCompiles(sources.glsl, true);

    auto executor = CpuCompute::Executor {graph.builder.graph()};
    expectPlans(executor);
};

auto tBuiltins = test("Float16Codegen/everyBuiltinIsSpelledOnHalves") = []
{
    auto graph = BuiltinGraph {};
    auto sources = Sources {graph.builder.graph()};

    checkAll(sources.metal,
             {"half3 eacpAsinh(half3 x)",
              "fma(t3, t3, half3(t6))",
              "atan2(t3, t6)",
              "clamp(t3, -1.0h, t6)",
              "cross(t3, (t3).yzx)",
              "half3(length(t3), distance(t3, (t3).zxy), dot(t3, t3))"});

    checkAll(sources.hlsl,
             {"eacpRoundHalf(sin(t3))",
              "eacpRoundHalf(eacpAsinh(t3))",
              "eacpRoundHalf(mad(t3, t3, t6))",
              "eacpRoundHalf(lerp(t3, eacpRoundHalf((t3 * 2.0)), 0.5))",
              "(floor(t3) + ceil(t3))",
              "(min(t3, t6) + max(t3, 0.25))",
              "eacpRoundHalf(frac(t3))"});
    checkNone(sources.hlsl, {"eacpRoundHalf(floor(", "eacpRoundHalf(min("});

    checkAll(sources.glsl,
             {"inversesqrt(", "atan(t3, f16vec3(t6))", "fma(t3, t3, f16vec3(t6))"});
    expectGlslCompiles(sources.glsl, true);

    auto executor = CpuCompute::Executor {graph.builder.graph()};
    expectPlans(executor);
};

template <int Columns, int Rows>
void checkShape(
    const std::source_location& location = std::source_location::current())
{
    auto shape = ShapeGraph<Columns, Rows> {};
    const auto& graph = shape.builder.graph();
    auto sources = Sources {graph};

    auto metalName = Strings::format("half{}x{}", Columns, Rows);
    auto hlslName = Strings::format("float{}x{}", Rows, Columns);
    auto glslName = Columns == Rows ? Strings::format("f16mat{}", Columns)
                                    : Strings::format("f16mat{}x{}", Columns, Rows);
    auto hlslRows = std::string {};

    for (auto row = 0; row < Rows; ++row)
        hlslRows +=
            Strings::format("{}eacpRoundHalf(x[{}])", row > 0 ? ", " : "", row);

    check(contains(sources.metal, metalName + "("), sources.metal, location);
    check(contains(sources.metal, metalName + " v1[2];"), sources.metal, location);
    check(!contains(sources.metal, "mul("), sources.metal, location);

    check(contains(sources.hlsl,
                   Strings::format("transpose(float{}x{}(", Columns, Rows)),
          sources.hlsl,
          location);
    check(contains(sources.hlsl, "eacpRoundHalf(mul("), sources.hlsl, location);
    check(contains(sources.hlsl,
                   Strings::format("{0} eacpRoundHalf({0} x)", hlslName)),
          sources.hlsl,
          location);
    check(contains(sources.hlsl,
                   Strings::format("return {}({});", hlslName, hlslRows)),
          sources.hlsl,
          location);

    check(contains(sources.glsl, glslName + "("), sources.glsl, location);
    expectGlslCompiles(sources.glsl, true, location);

    auto executor = CpuCompute::Executor {graph};
    expectPlans(executor, location);
}

auto tShapes = test("Float16Codegen/everyHalfMatrixShapeIsSpelled") = []
{
    checkShape<2, 2>();
    checkShape<2, 3>();
    checkShape<2, 4>();
    checkShape<3, 2>();
    checkShape<3, 3>();
    checkShape<3, 4>();
    checkShape<4, 2>();
    checkShape<4, 3>();
    checkShape<4, 4>();
};

auto tStatements = test("Float16Codegen/varsAndPrivateArraysHoldHalves") = []
{
    auto graph = StatementGraph {};
    auto sources = Sources {graph.builder.graph()};

    checkAll(sources.metal,
             {"half v0 = 0.0h;",
              "half v1[4];",
              "v1[0] = 0.125h;",
              "v0 = 0.25h;",
              "const half a0[2] = {0.5h, 1.5h};"});
    checkAll(sources.hlsl,
             {"float v0 = 0.0;",
              "float v1[4];",
              "const float a0[2] = {0.5, 1.5};",
              "v0 = eacpRoundHalf((v0 + eacpRoundHalf((v1[min(max(v2, 0), 3)] * "
              "0.5))));"});
    checkAll(sources.glsl,
             {"float16_t v0 = 0.0hf;",
              "float16_t v1[4];",
              "float16_t a0[2] = {0.5hf, 1.5hf};"});
    expectGlslCompiles(sources.glsl, true);

    auto executor = CpuCompute::Executor {graph.builder.graph()};
    expectPlans(executor);
};

auto tBuffers = test("Float16Codegen/halfBuffersAreTwoBytesAnElement") = []
{
    auto graph = ArithmeticGraph {};
    auto sources = Sources {graph.builder.graph()};

    checkAll(sources.metal,
             {"device const half* buffer0 [[buffer(0)]]",
              "device half* buffer2 [[buffer(2)]]",
              "buffer2[(t0 + 1u)] = (t6).y;"});

    checkAll(sources.hlsl,
             {"StructuredBuffer<uint> buffer0 : register(t0);",
              "RWStructuredBuffer<uint> buffer2 : register(u2);",
              "f16tof32(buffer0[(t0 + 1u) >> 1u] >> (((t0 + 1u) & 1u) << 4u))",
              "InterlockedAnd(buffer2[(t0) >> 1u], ~(0xffffu << (((t0) & 1u) << "
              "4u)));",
              "InterlockedOr(buffer2[(t0) >> 1u], f32tof16((t6).x) << (((t0) & 1u) "
              "<< 4u));"});

    checkAll(
        sources.glsl,
        {"#extension GL_EXT_shader_16bit_storage : require",
         "readonly buffer Buffer0\n{\n    float16_t buffer0[];",
         "buffer Buffer2\n{\n    float16_t buffer2[];",
         "f16vec4(buffer0[t0], buffer0[t0 + 1u], buffer0[t0 + 2u], buffer0[t0 + "
         "3u])"});
};

auto tExtensions = test("Float16Codegen/onlyAShaderUsingHalvesNamesThem") = []
{
    auto floats = FloatOnlyGraph {};
    auto floatSources = Sources {floats.builder.graph()};

    check(floatSources.glsl.starts_with("#version 450\n\nfloat eacpReadHalf("),
          floatSources.glsl);
    checkNone(floatSources.glsl, {"#extension", "float16_t"});
    checkNone(floatSources.hlsl, {"eacpRoundHalf"});
    check(!floats.builder.graph().usesHalf());

    auto arithmetic = HalfArithmeticOnlyGraph {};
    auto arithmeticSources = Sources {arithmetic.builder.graph()};

    check(arithmeticSources.glsl.starts_with(
              "#version 450\n#extension "
              "GL_EXT_shader_explicit_arithmetic_types_float16 : require\n\n"),
          arithmeticSources.glsl);
    checkNone(arithmeticSources.glsl, {"GL_EXT_shader_16bit_storage"});
    checkAll(arithmeticSources.hlsl, {"float eacpRoundHalf(float x)"});
    check(arithmetic.builder.graph().usesHalf());
    check(!arithmetic.builder.graph().usesHalfStorage());
    expectGlslCompiles(arithmeticSources.glsl, true);
};

auto tFragment = test("Float16Codegen/aFragmentComputesInHalves") = []
{
    auto graph = FragmentGraph {};
    auto sources = Sources {graph.builder.graph()};

    checkAll(sources.metal,
             {"device const half* buffer0 [[buffer(24)]]",
              "half2(input.v0) * buffer0[0u]",
              "float2 v0;"});
    checkAll(sources.hlsl,
             {"float2 eacpRoundHalf(float2 x)",
              "eacpRoundHalf(float2(input.v0))",
              "StructuredBuffer<uint> buffer0 : register(t8);"});
    checkAll(
        sources.glsl,
        {"layout(location = 0) out vec2 vary0;", "f16vec2(vary0) * buffer0[0u]"});
    expectGlslCompiles(sources.glsl);
};
} // namespace Float16CodegenTests
