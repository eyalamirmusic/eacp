#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

#include <cstdint>
#include <string>

// The unsigned integer vectors: a pair of indices held in one value, the four
// lanes of a hash, a thread position that is one handle rather than three.
//
// They exist on exactly the terms the signed ones do - the whole integer
// operator set componentwise, comparisons that yield a mask, min/max, the
// crossings to and from the other two families, and a place in a uniform block
// - and what separates them is that arithmetic wraps at 2^32 rather than
// overflowing a sign, which is what a hash and a checksum are written out of.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto uintBytes = (int) sizeof(std::uint32_t);
constexpr auto groupSize = ComputePass::threadGroupWidth;
constexpr auto groups = 4;
constexpr auto sharedThreads = groupSize * groups;

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

Buffer makeFilledUInts(int elements, std::uint32_t value)
{
    auto values = Vector<std::uint32_t> {};
    values.assign(elements, value);

    return Buffer {Device::shared(),
                   values.data(),
                   uintBytes * values.size(),
                   BufferUsage::Storage};
}

Vector<std::uint32_t> readUInts(const Buffer& buffer, int elements)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(elements);
    buffer.read(values.data(), uintBytes * elements);
    return values;
}

// Everything a uint2 and a uint4 can be asked componentwise, one thread's worth
// per record, so a wrong lane and a wrong operator are different wrong answers.
constexpr auto perThread = 28;

struct VectorArithmeticKernel final : ComputeProgram
{
    VectorArithmeticKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        auto a = uint2(i * 2654435761u, 4294967280u + i);
        auto b = uint2(i + 7u, 3u);
        auto q = uint4(i, i + 1u, i * 65537u, 4294967295u - i);

        auto sum = a + b;
        auto product = a * b;
        auto quotient = a / b;
        auto remainder = a % b;
        auto offset = 4294967295u - q;
        auto masked = q & 255u;
        auto rotated = (q << 4u) | (q >> 28u);
        auto smaller = min(a, b);
        auto larger = max(a, b);
        auto complement = ~q;

        auto record = i * (unsigned) perThread;

        write(output, record + 0u, sum.x());
        write(output, record + 1u, sum.y());
        write(output, record + 2u, product.x());
        write(output, record + 3u, product.y());
        write(output, record + 4u, masked.x());
        write(output, record + 5u, masked.y());
        write(output, record + 6u, masked.z());
        write(output, record + 7u, masked.w());
        write(output, record + 8u, rotated.x());
        write(output, record + 9u, rotated.y());
        write(output, record + 10u, rotated.z());
        write(output, record + 11u, rotated.w());
        write(output, record + 12u, smaller.x());
        write(output, record + 13u, smaller.y());
        write(output, record + 14u, larger.x());
        write(output, record + 15u, larger.y());
        write(output, record + 16u, complement.x());
        write(output, record + 17u, complement.y());
        write(output, record + 18u, complement.z());
        write(output, record + 19u, complement.w());
        write(output, record + 20u, quotient.x());
        write(output, record + 21u, quotient.y());
        write(output, record + 22u, remainder.x());
        write(output, record + 23u, remainder.y());
        write(output, record + 24u, offset.x());
        write(output, record + 25u, offset.y());
        write(output, record + 26u, offset.z());
        write(output, record + 27u, offset.w());
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

// The same arithmetic on the CPU, in the type the kernel computes in.
void expectedRecord(std::uint32_t i, std::uint32_t (&out)[perThread])
{
    const std::uint32_t a[] = {i * 2654435761u, 4294967280u + i};
    const std::uint32_t b[] = {i + 7u, 3u};
    const std::uint32_t q[] = {i, i + 1u, i * 65537u, 4294967295u - i};

    out[0] = a[0] + b[0];
    out[1] = a[1] + b[1];
    out[2] = a[0] * b[0];
    out[3] = a[1] * b[1];

    out[12] = a[0] < b[0] ? a[0] : b[0];
    out[13] = a[1] < b[1] ? a[1] : b[1];
    out[14] = a[0] > b[0] ? a[0] : b[0];
    out[15] = a[1] > b[1] ? a[1] : b[1];

    out[20] = a[0] / b[0];
    out[21] = a[1] / b[1];
    out[22] = a[0] % b[0];
    out[23] = a[1] % b[1];

    for (auto lane = 0; lane < 4; ++lane)
    {
        out[4 + lane] = q[lane] & 255u;
        out[8 + lane] = (q[lane] << 4u) | (q[lane] >> 28u);
        out[16 + lane] = ~q[lane];
        out[24 + lane] = 4294967295u - q[lane];
    }
}

// A pair and a triple sent from the CPU, used where a kernel would use them.
struct UniformPairKernel final : ComputeProgram
{
    UniformPairKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        auto moved = origin + uint2(i, i * 2u);
        auto scaled = step * i;

        write(output, i * 5u + 0u, moved.x());
        write(output, i * 5u + 1u, moved.y());
        write(output, i * 5u + 2u, scaled.x());
        write(output, i * 5u + 3u, scaled.y());
        write(output, i * 5u + 4u, scaled.z());
    }

    Uniform<UInt2> origin;
    Uniform<UInt3> step;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(origin, step, output)
};

// A pair carried across three turns of a loop, which is what a Var is for.
struct VarLoopKernel final : ComputeProgram
{
    VarLoopKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        auto accumulated = var(uint2(i, i + 1u));
        auto turn = var(0u);

        loop(turn < 3u,
             [&]
             {
                 accumulated = accumulated.get() * 2u + uint2(turn.get(), 1u);
                 turn += 1u;
             });

        write(output, i * 2u + 0u, accumulated.get().x());
        write(output, i * 2u + 1u, accumulated.get().y());
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

// Every thread reads the pair the opposite lane of its group wrote, so no
// thread reads anything it wrote itself.
struct SharedPairKernel final : ComputeProgram
{
    SharedPairKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto lane = localId();
        auto scratch = shared<UInt2>(groupSize);

        write(scratch, lane, uint2(id, id * 3u + 1u));
        barrier();

        auto opposite = scratch[(unsigned) (groupSize - 1) - lane];

        ifThen(id < gridCount(),
               [&]
               {
                   write(output, id * 2u + 0u, opposite.x());
                   write(output, id * 2u + 1u, opposite.y());
               });
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tUIntVectorConstructAndSwizzle = test("UIntVector/aPairIsBuiltAndSwizzled") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto pair = uint2(i, 3u);
    auto swapped = pair.yx();

    builder.write(output, i, swapped.x() + pair.y());

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "uint2 t0 = uint2(gid, 3u);"));
        check(contains(source, "buffer0[gid] = (((t0).yx).x + (t0).y);"));
    }

    expectGlslCompiles(builder.graph());
};

auto tUIntVectorTripleSwizzle = test("UIntVector/aQuadNamesThreeOfItsLanes") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto lanes = uint4(i, i + 1u, 2u, 3u);

    builder.write(output, i, lanes.xyz().z() + lanes.w());

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "uint4 t0 = uint4(gid, (gid + 1u), 2u, 3u);"));
        check(contains(source, "buffer0[gid] = (((t0).xyz).z + (t0).w);"));
    }

    expectGlslCompiles(builder.graph());
};

auto tUIntVectorOperators = test("UIntVector/everyOperatorIsComponentwise") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto a = uint2(i, 3u);
    auto b = uint2(i, 5u);

    builder.write(output, 0u, (a + b).x());
    builder.write(output, 1u, (a - b).x());
    builder.write(output, 2u, (a * b).x());
    builder.write(output, 3u, (a / b).x());
    builder.write(output, 4u, (a % b).x());
    builder.write(output, 5u, (a & b).x());
    builder.write(output, 6u, (a | b).x());
    builder.write(output, 7u, (a ^ b).x());
    builder.write(output, 8u, (a << 1u).x());
    builder.write(output, 9u, (a >> 1u).x());
    builder.write(output, 10u, (~a).x());
    builder.write(output, 11u, (a * i).x());
    builder.write(output, 12u, min(a, b).x());
    builder.write(output, 13u, max(a, b).x());

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "buffer0[0u] = ((t0 + t1)).x;"));
        check(contains(source, "buffer0[1u] = ((t0 - t1)).x;"));
        check(contains(source, "buffer0[2u] = ((t0 * t1)).x;"));
        check(contains(source, "buffer0[3u] = ((t0 / t1)).x;"));
        check(contains(source, "buffer0[4u] = ((t0 % t1)).x;"));
        check(contains(source, "buffer0[5u] = ((t0 & t1)).x;"));
        check(contains(source, "buffer0[6u] = ((t0 | t1)).x;"));
        check(contains(source, "buffer0[7u] = ((t0 ^ t1)).x;"));
        check(contains(source, "buffer0[8u] = ((t0 << 1u)).x;"));
        check(contains(source, "buffer0[9u] = ((t0 >> 1u)).x;"));
        check(contains(source, "buffer0[10u] = ((~(t0))).x;"));
        check(contains(source, "buffer0[11u] = ((t0 * gid)).x;"));
        check(contains(source, "buffer0[12u] = (min(t0, t1)).x;"));
        check(contains(source, "buffer0[13u] = (max(t0, t1)).x;"));
    }

    expectGlslCompiles(builder.graph());
};

// GLSL refuses a scalar on the left of a shift whose right operand is a vector,
// where MSL and HLSL broadcast it themselves, so the emitter writes the
// constructor in.
auto tUIntVectorScalarLeftShift =
    test("UIntVector/aScalarShiftedByAPairIsBroadcastInGlsl") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto pair = uint2(i, 3u);

    builder.write(output, 0u, (1u << pair).x());
    builder.write(output, 1u, (i >> pair).y());

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "(1u << t0)"));
        check(contains(source, "(gid >> t0)"));
    }

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "(uvec2(1u) << t0)"));
    check(contains(glsl, "(uvec2(gid) >> t0)"));
    check(!contains(glsl, "(1u << t0)"));

    expectGlslCompiles(builder.graph());
};

// A varying carries an unsigned vector as flat as it carries a signed one:
// GLSL rejects a non-flat integer stage input outright.
auto tUIntVectorFlatVarying = test("UIntVector/anUnsignedVaryingIsFlat") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto cell = builder.varying(toUInt(position * 8.0f));

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(toFloat(cell) * 0.125f, 0.0f, 1.0f));

    check(contains(emitMetal(builder.graph()), "    uint2 v0 [[flat]];\n"));
    check(contains(emitHlsl(builder.graph()),
                   "    nointerpolation uint2 v0 : TEXCOORD0;\n"));

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "layout(location = 0) flat out uvec2 vary0;\n"));
    check(contains(glsl, "layout(location = 0) flat in uvec2 vary0;\n"));

    expectGlslCompiles(builder.graph());
};

auto tUIntVectorComparisonYieldsAMask =
    test("UIntVector/aComparisonYieldsAMaskAnyCollapses") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto a = uint2(i, 3u);
    auto b = uint2(i, 5u);
    auto anyBelow = any(a < b);

    builder.write(output, i, select(anyBelow, 1u, 0u));

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
        check(contains(
            source,
            "buffer0[gid] = (any((uint2(gid, 3u) < uint2(gid, 5u))) ? 1u : 0u);"));

    expectGlslCompiles(builder.graph());
};

auto tUIntVectorSelect = test("UIntVector/selectPicksBetweenTwoPairs") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto picked = select(i > 4u, uint2(i, 1u), uint2(2u, i));

    builder.write(output, i, picked.x() + picked.y());

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
        check(contains(
            source, "uint2 t0 = ((gid > 4u) ? uint2(gid, 1u) : uint2(2u, gid));"));

    expectGlslCompiles(builder.graph());
};

auto tUIntVectorConversions = test("UIntVector/theCrossingsAreNamedCasts") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto cell = toUInt(input.read2(i));
    auto signedCell = toInt(cell);
    auto widened = toFloat(cell);

    builder.write(output, 0u, cell.x());
    builder.write(output, 1u, toUInt(signedCell).y());
    builder.write(output, 2u, toUInt(widened).x());

    // The pair itself is one load on Metal and two subscripts on HLSL; what
    // this is about is the conversion around it, which is a named cast either
    // way and is spelled over the vector rather than per component.
    check(contains(emitMetal(builder.graph()),
                   "uint2 t1 = uint2(float2(*((device const packed_float2*) "));
    check(contains(emitHlsl(builder.graph()),
                   "uint2 t1 = uint2(float2(buffer0[t0], "));

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "buffer1[1u] = (uint2(int2(t1))).y;"));
        check(contains(source, "buffer1[2u] = (uint2(float2(t1))).x;"));
    }

    expectGlslCompiles(builder.graph());
};

auto tUIntVectorBitcasts = test("UIntVector/theBitcastsUseTheVectorSpelling") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto bits = asUInt(input.read2(i));
    auto back = asFloat(bits);

    builder.write(output, 0u, bits.x());
    builder.write(output, 1u, toUInt(back.y()));

    auto metal = emitMetal(builder.graph());
    auto hlsl = emitHlsl(builder.graph());

    check(contains(metal,
                   "uint2 t1 = as_type<uint2>(float2(*((device const "
                   "packed_float2*) (buffer0 + t0))));"));
    check(contains(metal, "buffer1[1u] = uint((as_type<float2>(t1)).y);"));

    check(contains(hlsl, "uint2 t1 = asuint(float2(buffer0[t0], "));
    check(contains(hlsl, "buffer1[1u] = uint((asfloat(t1)).y);"));
    check(!contains(hlsl, "as_type"));

    expectGlslCompiles(builder.graph());
};

auto tUIntVectorUniformPacking = test("UIntVector/aTripleUniformPadsOnlyOnHlsl") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto scale = builder.uniform<Float>();
    auto step = builder.uniform<UInt3>();

    builder.write(output, i, step.x() + toUInt(scale));

    auto metal = emitMetal(builder.graph());
    auto hlsl = emitHlsl(builder.graph());

    check(contains(metal, "    float u0;\n    uint3 u1;\n"));
    check(!contains(metal, "pad"));

    check(contains(hlsl, "    float u0;\n    float pad0;\n    uint3 u1;\n"));

    expectGlslCompiles(builder.graph());
};

auto tUIntVectorArithmeticRuns = test("UIntVector/wrapsAndMasksExactly") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 128;

    auto output = makeFilledUInts(threads * perThread, 0u);

    auto kernel = VectorArithmeticKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readUInts(output, threads * perThread);
    auto wrapped = 0;

    for (auto thread = 0; thread < threads; ++thread)
    {
        std::uint32_t expected[perThread] = {};
        expectedRecord((std::uint32_t) thread, expected);

        for (auto slot = 0; slot < perThread; ++slot)
            check(values[thread * perThread + slot] == expected[slot]);

        if (expected[1] < 4294967280u + (std::uint32_t) thread)
            ++wrapped;
    }

    // And the wraparound past 2^32 is genuinely exercised rather than assumed.
    check(wrapped > 0);
};

auto tUIntVectorUniformsRun = test("UIntVector/uniformPairsArriveAsSent") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 32;

    auto output = makeFilledUInts(threads * 5, 0u);

    auto kernel = UniformPairKernel {};
    kernel.origin = {4000000000u, 17u};
    kernel.step = {3u, 5u, 7u};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readUInts(output, threads * 5);

    for (auto thread = 0; thread < threads; ++thread)
    {
        auto i = (std::uint32_t) thread;

        check(values[thread * 5 + 0] == 4000000000u + i);
        check(values[thread * 5 + 1] == 17u + i * 2u);
        check(values[thread * 5 + 2] == 3u * i);
        check(values[thread * 5 + 3] == 5u * i);
        check(values[thread * 5 + 4] == 7u * i);
    }
};

auto tUIntVectorVarRuns = test("UIntVector/aPairAdvancesThroughALoop") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 64;

    auto output = makeFilledUInts(threads * 2, 0u);

    auto kernel = VarLoopKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readUInts(output, threads * 2);

    for (auto thread = 0; thread < threads; ++thread)
    {
        auto i = (std::uint32_t) thread;
        std::uint32_t accumulated[] = {i, i + 1u};

        for (auto turn = 0u; turn < 3u; ++turn)
        {
            accumulated[0] = accumulated[0] * 2u + turn;
            accumulated[1] = accumulated[1] * 2u + 1u;
        }

        check(values[thread * 2 + 0] == accumulated[0]);
        check(values[thread * 2 + 1] == accumulated[1]);
    }
};

auto tUIntVectorSharedRuns = test("UIntVector/sharedPairsCrossLanes") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeFilledUInts(sharedThreads * 2, 0u);

    auto kernel = SharedPairKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, sharedThreads);
    }

    commands.commit();

    auto values = readUInts(output, sharedThreads * 2);

    for (auto thread = 0; thread < sharedThreads; ++thread)
    {
        auto base = (thread / groupSize) * groupSize;
        auto opposite = (std::uint32_t) (base + groupSize - 1 - thread % groupSize);

        check(values[thread * 2 + 0] == opposite);
        check(values[thread * 2 + 1] == opposite * 3u + 1u);
    }

    // A reversal rather than the identity, which unshared scratch would give.
    check(values[0] != 0u);
};
