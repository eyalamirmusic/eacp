#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

#include <cstdint>
#include <string>

// The scalar UInt on the terms its own vectors and the signed scalar already
// hold: the bitwise set, the two shifts, the complement, and an unsigned
// literal that is a handle in its own right.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto uintBytes = (int) sizeof(std::uint32_t);

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

Buffer makeUInts(int elements)
{
    auto values = Vector<std::uint32_t> {};
    values.assign(elements, 0u);

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

// One thread's worth per record, every operator in every form it takes, so a
// wrong form and a wrong operator are different wrong answers.
constexpr auto perThread = 16;

struct ScalarBitsKernel final : ComputeProgram
{
    ScalarBitsKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto seed = i * 2654435761u + 1u;
        auto turn = i % 31u;

        auto record = i * (unsigned) perThread;

        write(output, record + 0u, seed & 255u);
        write(output, record + 1u, 255u & (seed >> 24u));
        write(output, record + 2u, seed & i);
        write(output, record + 3u, seed | i);
        write(output, record + 4u, seed ^ i);
        write(output, record + 5u, (seed << 7u) | (seed >> 25u));
        write(output, record + 6u, (seed ^ (seed >> 16u)) ^ (seed >> 8u));
        write(output, record + 7u, ~seed);
        write(output, record + 8u, 1u << turn);
        write(output, record + 9u, seed | 4096u);
        write(output, record + 10u, 4096u | seed);
        write(output, record + 11u, seed ^ 61u);
        write(output, record + 12u, 61u ^ seed);
        write(output, record + 13u, seed >> turn);
        write(output, record + 14u, 4294967295u >> turn);
        write(output, record + 15u, seed << 3u);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

// The same on the CPU, in the type the kernel computes in.
void expectedRecord(std::uint32_t i, std::uint32_t (&out)[perThread])
{
    const auto seed = i * 2654435761u + 1u;
    const auto turn = i % 31u;

    out[0] = seed & 255u;
    out[1] = 255u & (seed >> 24u);
    out[2] = seed & i;
    out[3] = seed | i;
    out[4] = seed ^ i;
    out[5] = (seed << 7u) | (seed >> 25u);
    out[6] = (seed ^ (seed >> 16u)) ^ (seed >> 8u);
    out[7] = ~seed;
    out[8] = 1u << turn;
    out[9] = seed | 4096u;
    out[10] = 4096u | seed;
    out[11] = seed ^ 61u;
    out[12] = 61u ^ seed;
    out[13] = seed >> turn;
    out[14] = 4294967295u >> turn;
    out[15] = seed << 3u;
}

// A pair with no handle in it but the literals themselves, and a bare unsigned
// held in a Var without an arithmetic detour to anchor it.
struct LiteralPairKernel final : ComputeProgram
{
    LiteralPairKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        auto pair = uint2(unsignedInteger(11u), unsignedInteger(22u));
        auto held = var(unsignedInteger(48u));

        held = held.get() | 5u;

        write(output, i * 3u + 0u, pair.x());
        write(output, i * 3u + 1u, pair.y());
        write(output, i * 3u + 2u, held.get());
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tUIntScalarBitwiseSet = test("UIntScalar/theBitwiseSetTakesEitherSide") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.uintInputBuffer();
    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();
    auto bits = input[i];

    builder.write(output, 0u, bits & i);
    builder.write(output, 1u, bits & 255u);
    builder.write(output, 2u, 255u & bits);
    builder.write(output, 3u, bits | i);
    builder.write(output, 4u, bits | 255u);
    builder.write(output, 5u, 255u | bits);
    builder.write(output, 6u, bits ^ i);
    builder.write(output, 7u, bits ^ 255u);
    builder.write(output, 8u, 255u ^ bits);
    builder.write(output, 9u, ~bits);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "buffer1[0u] = (t0 & gid);"));
        check(contains(source, "buffer1[1u] = (t0 & 255u);"));
        check(contains(source, "buffer1[2u] = (255u & t0);"));
        check(contains(source, "buffer1[3u] = (t0 | gid);"));
        check(contains(source, "buffer1[4u] = (t0 | 255u);"));
        check(contains(source, "buffer1[5u] = (255u | t0);"));
        check(contains(source, "buffer1[6u] = (t0 ^ gid);"));
        check(contains(source, "buffer1[7u] = (t0 ^ 255u);"));
        check(contains(source, "buffer1[8u] = (255u ^ t0);"));
        check(contains(source, "buffer1[9u] = (~(t0));"));
    }

    expectGlslCompiles(builder.graph());
};

auto tUIntScalarShifts = test("UIntScalar/theShiftsTakeEitherSide") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.uintInputBuffer();
    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();
    auto bits = input[i];

    builder.write(output, 0u, bits << i);
    builder.write(output, 1u, bits << 4u);
    builder.write(output, 2u, 1u << bits);
    builder.write(output, 3u, bits >> i);
    builder.write(output, 4u, bits >> 28u);
    builder.write(output, 5u, 2147483647u >> bits);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "buffer1[0u] = (t0 << gid);"));
        check(contains(source, "buffer1[1u] = (t0 << 4u);"));
        check(contains(source, "buffer1[2u] = (1u << t0);"));
        check(contains(source, "buffer1[3u] = (t0 >> gid);"));
        check(contains(source, "buffer1[4u] = (t0 >> 28u);"));
        check(contains(source, "buffer1[5u] = (2147483647u >> t0);"));
    }

    expectGlslCompiles(builder.graph());
};

// The graph holds a uint constant in the int the three literal kinds share, so
// everything above INT_MAX is negative in storage and must print as what it was.
auto tUIntScalarLargeLiteral =
    test("UIntScalar/aLiteralAboveIntMaxPrintsUnsigned") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    builder.write(output, 0u, 4294967295u - i);
    builder.write(output, 1u, i & 4294967295u);
    builder.write(output, 2u, 4294967295u >> i);
    builder.write(output, 3u, builder.unsignedInteger(3000000000u));

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "buffer0[0u] = (4294967295u - gid);"));
        check(contains(source, "buffer0[1u] = (gid & 4294967295u);"));
        check(contains(source, "buffer0[2u] = (4294967295u >> gid);"));
        check(contains(source, "buffer0[3u] = 3000000000u;"));
        check(!contains(source, "-1u"));
        check(!contains(source, "-1294967296u"));
    }

    expectGlslCompiles(builder.graph());
};

auto tUIntScalarLiteral = test("UIntScalar/anUnsignedLiteralIsAHandle") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto pair = uint2(builder.unsignedInteger(11u), builder.unsignedInteger(22u));
    auto held = builder.var(builder.unsignedInteger(48u));

    held = held.get() | 5u;

    builder.write(output, i, held.get() + pair.x() + pair.y());

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "uint2 t0 = uint2(11u, 22u);"));
        check(contains(source, "uint v0 = 48u;"));
        check(contains(source, "v0 = (v0 | 5u);"));
    }

    expectGlslCompiles(builder.graph());
};

auto tUIntScalarBitsRun = test("UIntScalar/masksRotatesAndFoldsExactly") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 128;

    auto output = makeUInts(threads * perThread);

    auto kernel = ScalarBitsKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readUInts(output, threads * perThread);

    for (auto thread = 0; thread < threads; ++thread)
    {
        std::uint32_t expected[perThread] = {};
        expectedRecord((std::uint32_t) thread, expected);

        for (auto slot = 0; slot < perThread; ++slot)
            check(values[thread * perThread + slot] == expected[slot]);
    }
};

auto tUIntScalarLiteralRuns = test("UIntScalar/aWhollyLiteralPairArrives") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 32;

    auto output = makeUInts(threads * 3);

    auto kernel = LiteralPairKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readUInts(output, threads * 3);

    for (auto thread = 0; thread < threads; ++thread)
    {
        check(values[thread * 3 + 0] == 11u);
        check(values[thread * 3 + 1] == 22u);
        check(values[thread * 3 + 2] == 53u);
    }
};
