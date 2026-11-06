#include "CodegenCommon.h"

#include <eacp/Core/Utils/Format.h>

#include <cstring>
#include <initializer_list>
#include <source_location>
#include <string>

// The types of WGSL's stage 9, part A, as the three dialects spell them. A
// non-square matrix is named columns first on Metal and GLSL (float2x3,
// mat2x3) and rows first on HLSL (float3x2), and HLSL builds one from the rows
// of the transposed shape and transposes it, as it does a square one; a column
// is a subscript, of the transpose on HLSL. Every matrix crosses as a uniform
// at a 16-byte column stride: Metal holds a two-row one as float4 columns,
// HLSL pads after the last column's register. A private array is declared bare
// and filled in a loop, and every dynamic index is clamped where it is
// recorded. The i32 buffers are int-typed bindings. The GLSL is compiled
// wherever eacp-spirv is built; the numbers are pinned by CpuComputeTests'
// Types/ and on a device by TypesDevice/.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace TypesCodegenTests
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

// The vertex function's text and the fragment function's.
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
        check(contains(source, text), std::string(text) + "\n" + source, location);
}

template <int Width>
FloatVectorOf<Width> columnOf(const InputBuffer& values, unsigned first)
{
    auto at = [&](unsigned offset) { return values[first + offset]; };

    if constexpr (Width == 2)
        return float2(at(0u), at(1u));
    else if constexpr (Width == 3)
        return float3(at(0u), at(1u), at(2u));
    else
        return float4(at(0u), at(1u), at(2u), at(3u));
}

template <int Columns, int Rows>
MatrixOf<Columns, Rows> matrixOf(const InputBuffer& values)
{
    auto column = [&](int which)
    { return columnOf<Rows>(values, (unsigned) (which * Rows)); };

    if constexpr (Columns == 2 && Rows == 3)
        return float2x3(column(0), column(1));
    else if constexpr (Columns == 2 && Rows == 4)
        return float2x4(column(0), column(1));
    else if constexpr (Columns == 3 && Rows == 2)
        return float3x2(column(0), column(1), column(2));
    else if constexpr (Columns == 3 && Rows == 4)
        return float3x4(column(0), column(1), column(2));
    else if constexpr (Columns == 4 && Rows == 2)
        return float4x2(column(0), column(1), column(2), column(3));
    else
        return float4x3(column(0), column(1), column(2), column(3));
}

template <int Width>
void writeVector(ShaderBuilder& builder,
                 const OutputBuffer& output,
                 unsigned& at,
                 const FloatVectorOf<Width>& value)
{
    auto next = [&] { return builder.unsignedInteger(at++); };

    builder.write(output, next(), value.x());
    builder.write(output, next(), value.y());

    if constexpr (Width > 2)
        builder.write(output, next(), value.z());

    if constexpr (Width > 3)
        builder.write(output, next(), value.w());
}

// One kernel per shape: built from columns, a fixed and a dynamic column,
// transposed, both vector products, the product with its own transpose, the
// scale, the sum and the difference.
template <int Columns, int Rows>
struct ShapeGraph
{
    ShapeGraph()
    {
        auto values = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();
        auto at = 0u;

        auto m = matrixOf<Columns, Rows>(values);
        auto byColumns = columnOf<Columns>(values, 20u);
        auto byRows = columnOf<Rows>(values, 24u);
        auto square = m * transpose(m);
        auto sum = (m + m * 2.f - m / 4.f) * byColumns;

        writeVector<Rows>(builder, output, at, m[1]);
        writeVector<Rows>(builder, output, at, m[toInt(id) - 1]);
        writeVector<Rows>(builder, output, at, m * byColumns);
        writeVector<Columns>(builder, output, at, byRows * m);
        writeVector<Rows>(builder, output, at, square[0]);
        writeVector<Columns>(builder, output, at, transpose(m)[id]);
        writeVector<Rows>(builder, output, at, sum);
    }

    ShaderBuilder builder;
};

std::string msl(int columns, int rows)
{
    return Strings::format("float{}x{}", columns, rows);
}

std::string glslName(int columns, int rows)
{
    return Strings::format("mat{}x{}", columns, rows);
}

template <int Columns, int Rows>
void checkShape(
    const std::source_location& location = std::source_location::current())
{
    auto shape = ShapeGraph<Columns, Rows> {};
    const auto& graph = shape.builder.graph();
    auto sources = Sources {graph};

    auto construct = msl(Columns, Rows) + "(";
    auto hlslConstruct = "transpose(" + construct;
    auto hlslLocal = msl(Rows, Columns) + " t";

    check(contains(sources.metal, construct), sources.metal, location);
    check(contains(sources.metal, ")[1]"), sources.metal, location);
    check(contains(sources.metal, "min(max("), sources.metal, location);
    check(!contains(sources.metal, "mul("), sources.metal, location);

    check(contains(sources.hlsl, hlslConstruct), sources.hlsl, location);
    check(contains(sources.hlsl, hlslLocal), sources.hlsl, location);
    check(contains(sources.hlsl, "mul("), sources.hlsl, location);
    check(contains(sources.hlsl, ")[1]"), sources.hlsl, location);

    check(contains(sources.glsl, glslName(Columns, Rows) + "("),
          sources.glsl,
          location);
    check(contains(sources.glsl, "transpose("), sources.glsl, location);

    expectGlslCompiles(sources.glsl, true, location);

    auto executor = CpuCompute::Executor {graph};
    expectPlans(executor, location);
}

struct UniformGraph
{
    UniformGraph()
    {
        auto output = builder.outputBuffer();
        auto id = builder.threadId();
        auto a = builder.uniform<Float2x3>();
        auto s = builder.uniform<Float>();
        auto b = builder.uniform<Float3x2>();
        auto c = builder.uniform<Float2x2>();
        auto t = builder.uniform<Float>();

        auto picked = a[0].x() + s + b[2].y() + c[1].x() + t;
        builder.write(output, id, picked);
    }

    ShaderBuilder builder;
};

// Every uniform shape, between scalars, read back element for element.
struct UniformKernel final : ComputeKernel
{
    UniformKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        write(output, id, a[1].z() + s + b[2].y() + c[1].x() + t);
    }

    Uniform<Float2x3> a;
    Uniform<Float> s;
    Uniform<Float3x2> b;
    Uniform<Float2x2> c;
    Uniform<Float> t;
    Uniform<OutputBuffer> output;
    EACP_SHADER(a, s, b, c, t, output)
};

float floatAt(const void* bytes, int offset)
{
    auto value = 0.f;
    std::memcpy(
        &value, static_cast<const std::byte*>(bytes) + offset, sizeof(value));
    return value;
}

struct PrivateArrayGraph
{
    PrivateArrayGraph()
    {
        auto output = builder.outputBuffer();
        auto id = builder.threadId();

        auto items = builder.privateArray<Float, 4>();
        items.set(toInt(id) - 2, toFloat(id));
        items.set(id, items[toInt(id)] + 1.f);

        auto matrices = builder.privateArray<Float2x3, 2>();
        auto column = float3(toFloat(id), 1.f, 2.f);
        matrices.set(1, float2x3(column, column));

        builder.write(output, id, items[id] + matrices[id % 2u][1].y());
    }

    ShaderBuilder builder;
};

// A private array in each render stage: the vertex one picks an x by vertex
// id, the fragment one a shade by the fragment's column.
struct StageArrayGraph
{
    StageArrayGraph()
    {
        auto position = builder.vertexInput<Float2>();

        auto place = [&]
        {
            auto xs = builder.privateArray(builder.constant(-1.f),
                                           builder.constant(0.f),
                                           builder.constant(1.f));
            builder.position(
                float4(xs[builder.vertexId() % 3u], position.y(), 0.f, 1.f));
        };
        builder.vertexStage(place);

        auto shades = builder.privateArray<Float, 4>();
        auto column = toInt(builder.fragmentCoordinate().x());
        shades.set(column, builder.constant(0.5f));
        builder.fragment(float4(shades[column], 0.f, 0.f, 1.f));
    }

    ShaderBuilder builder;
};

struct IntBufferGraph
{
    IntBufferGraph()
    {
        auto input = builder.intInputBuffer();
        auto output = builder.intOutputBuffer();
        auto id = builder.threadId();

        builder.write(output, id, input[id] + input.read4(id).w());
        builder.write2(output, id, input.read2(id));
        builder.write(output, id, output.read3(id));
        builder.write(output, 0u, -7);
    }

    ShaderBuilder builder;
};

struct IntFragmentGraph
{
    IntFragmentGraph()
    {
        auto position = builder.vertexInput<Float2>();
        auto offsets = builder.intInputBuffer();
        builder.position(float4(position, 0.f, 1.f));
        builder.fragment(float4(toFloat(offsets[0u]), 0.f, 0.f, 1.f));
    }

    ShaderBuilder builder;
};
} // namespace

auto tTypeNames = test("TypesCodegen/eachShapeIsSpelledColumnsFirstSaveOnHlsl") = []
{
    check(std::string(typeName(ValueType::Float2x3)) == "float2x3");
    check(std::string(hlslTypeName(ValueType::Float2x3)) == "float3x2");
    check(std::string(glslTypeName(ValueType::Float2x3)) == "mat2x3");
    check(std::string(hlslTypeName(ValueType::Float4x4)) == "float4x4");
    check(componentCount(ValueType::Float4x3) == 12);
    check(matrixColumns(ValueType::Float4x3) == 4);
    check(matrixRows(ValueType::Float4x3) == 3);
    check(transposedMatrix(ValueType::Float2x4) == ValueType::Float4x2);
    check(matrixOrder(ValueType::Float3x3) == 3);
    check(matrixOrder(ValueType::Float3x2) == 0);
};

auto tShapes = test("TypesCodegen/everyShapesOperationsPrintInEachDialect") = []
{
    checkShape<2, 3>();
    checkShape<2, 4>();
    checkShape<3, 2>();
    checkShape<3, 4>();
    checkShape<4, 2>();
    checkShape<4, 3>();
};

auto tUniformBlock = test("TypesCodegen/uniformMatricesSitAtASixteenByteStride") = []
{
    auto shape = UniformGraph {};
    auto sources = Sources {shape.builder.graph()};

    checkAll(sources.metal,
             {"    float2x3 u0;\n    float u1;\n    float4 u2[3];\n"
              "    float4 u3[2];\n    float u4;\n",
              "float3x2(uniforms.u2[0].xy, uniforms.u2[1].xy, uniforms.u2[2].xy)",
              "float2x2(uniforms.u3[0].xy, uniforms.u3[1].xy)"});

    checkAll(sources.hlsl,
             {"    float3x2 u0;\n    float pad0;\n    float u1;\n"
              "    float2x3 u2;\n    float2x2 u3;\n    float pad1;\n"
              "    float pad2;\n    float u4;\n"});

    checkAll(sources.glsl,
             {"    mat2x3 u0;\n    float u1;\n    mat3x2 u2;\n    mat2 u3;\n"
              "    float u4;\n"});

    auto types = shape.builder.graph().uniforms();
    auto offsets = uniformOffsets(types);
    check(offsets[0] == 0 && offsets[1] == 32 && offsets[2] == 48);
    check(offsets[3] == 96 && offsets[4] == 128);
    check(uniformBlockSize(types) == 144);
    check(uniformPackedSize(ValueType::Float3x3, false) == 44);
    check(uniformPackedSize(ValueType::Float3x3, true) == 48);

    expectGlslCompiles(sources.glsl, true);
};

auto tUniformUpload = test("TypesCodegen/theUploadWalkSpreadsEachColumn") = []
{
    auto kernel = UniformKernel {};
    kernel.a = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
    kernel.s = 7.f;
    kernel.b = {8.f, 9.f, 10.f, 11.f, 12.f, 13.f};
    kernel.c = {14.f, 15.f, 16.f, 17.f};
    kernel.t = 18.f;

    const auto* bytes = kernel.packedUniforms(5);
    check(kernel.uniformByteSize() == 144);

    auto expect = [&](int offset, float value)
    { check(floatAt(bytes, offset) == value, std::to_string(offset)); };

    expect(0, 1.f);
    expect(8, 3.f);
    expect(16, 4.f);
    expect(24, 6.f);
    expect(32, 7.f);
    expect(48, 8.f);
    expect(52, 9.f);
    expect(64, 10.f);
    expect(80, 12.f);
    expect(84, 13.f);
    expect(96, 14.f);
    expect(112, 16.f);
    expect(116, 17.f);
    expect(128, 18.f);

    auto executor = CpuCompute::Executor {kernel};
    expectPlans(executor);

    auto output = filledWith(3, 0.f);
    auto bindings = CpuCompute::Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, 3));
    check(output[2] == 6.f + 7.f + 13.f + 16.f + 18.f);
};

auto tPrivateArray =
    test("TypesCodegen/aPrivateArrayIsFilledThenIndexedClamped") = []
{
    auto shape = PrivateArrayGraph {};
    const auto& graph = shape.builder.graph();
    auto sources = Sources {graph};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
        checkAll(*source,
                 {"for (int fill0 = 0; fill0 < 4; ++fill0)\n",
                  "        v0[fill0] = 0.0;\n",
                  "for (int fill1 = 0; fill1 < 2; ++fill1)\n",
                  "min(max(",
                  ", 0), 3)]",
                  ", 3u)]",
                  ", 1u)]",
                  "    v1[1] = "});

    checkAll(sources.metal, {"    float v0[4];\n", "    float2x3 v1[2];\n"});
    checkAll(sources.hlsl, {"    float v0[4];\n", "    float3x2 v1[2];\n"});
    checkAll(sources.glsl, {"    float v0[4];\n", "    mat2x3 v1[2];\n"});

    expectGlslCompiles(sources.glsl, true);

    auto executor = CpuCompute::Executor {graph};
    expectPlans(executor);
};

auto tStageArrays = test("TypesCodegen/eachStageDeclaresItsOwnPrivateArray") = []
{
    auto shape = StageArrayGraph {};
    auto sources = Sources {shape.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        auto stages = Stages {*source};
        checkAll(stages.vertex, {"v0[3];\n", "v0[0] = -1.0;\n", "v0[2] = 1.0;\n"});
        checkAll(stages.fragment, {"v1[4];\n", "v1[fill1] = 0.0;\n"});
        check(!contains(stages.fragment, "v0["), stages.fragment);
        check(!contains(stages.vertex, "v1["), stages.vertex);
    }

    expectGlslCompiles(sources.glsl);
};

auto tIntBuffers = test("TypesCodegen/intBuffersAreIntTypedBindings") = []
{
    auto shape = IntBufferGraph {};
    const auto& graph = shape.builder.graph();
    auto sources = Sources {graph};

    checkAll(sources.metal,
             {"device const int* buffer0", "device int* buffer1", "packed_int4"});
    checkAll(sources.hlsl,
             {"StructuredBuffer<int> buffer0", "RWStructuredBuffer<int> buffer1"});
    checkAll(sources.glsl, {"readonly buffer Buffer0", "int buffer0[];"});

    expectGlslCompiles(sources.glsl, true);

    auto executor = CpuCompute::Executor {graph};
    expectPlans(executor);
};

auto tIntFragment = test("TypesCodegen/aFragmentReadsAnIntBuffer") = []
{
    auto shape = IntFragmentGraph {};
    auto sources = Sources {shape.builder.graph()};

    checkAll(Stages {sources.metal}.fragment, {"device const int* buffer0"});
    checkAll(sources.hlsl, {"StructuredBuffer<int> buffer0"});
    checkAll(sources.glsl, {"int buffer0[];"});

    expectGlslCompiles(sources.glsl);
};
} // namespace TypesCodegenTests
