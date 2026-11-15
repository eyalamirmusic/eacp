#include "CodegenCommon.h"

#include <eacp/Core/Utils/Format.h>

#include <array>
#include <climits>
#include <string>

// WGSL's atomics as the three dialects spell them: every read-modify-write and
// the compare-exchange on an AtomicBuffer, an IntAtomicBuffer and the two
// SharedAtomic forms. HLSL and GLSL have no subtraction and add the negated
// operand; HLSL hands every old value back through an out parameter; Metal's
// compare-exchange is weak and writes the old value into the comparand's
// place. The GLSL is compiled wherever eacp-spirv is built. What the numbers
// are is pinned on the CPU by CpuComputeTests' AtomicOpsTests and on a device
// by AtomicOpsDeviceTests.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace AtomicOpsCodegenTests
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

template <typename T>
UInt wordOf(const T& value)
{
    if constexpr (std::same_as<T, Int>)
        return toUInt(value);
    else
        return value;
}

// Every operation once, at literal element 3 with literal operands, each old
// value written out so none is dead. The variables come out as v0..v7 for the
// read-modify-writes in this order, then v8 and v9 for the compare-exchange.
template <AtomicStorage Target>
void recordEveryOperation(ShaderBuilder& builder,
                          const Target& target,
                          const UIntOutputBuffer& output,
                          const AtomicOperand<AtomicValue<Target>>& operand,
                          const AtomicOperand<AtomicValue<Target>>& comparand)
{
    auto results = Vector<UInt> {};
    results.add(wordOf(builder.atomicAdd(target, 3u, operand)));
    results.add(wordOf(builder.atomicSub(target, 3u, operand)));
    results.add(wordOf(builder.atomicMax(target, 3u, operand)));
    results.add(wordOf(builder.atomicMin(target, 3u, operand)));
    results.add(wordOf(builder.atomicAnd(target, 3u, operand)));
    results.add(wordOf(builder.atomicOr(target, 3u, operand)));
    results.add(wordOf(builder.atomicXor(target, 3u, operand)));
    results.add(wordOf(builder.atomicExchange(target, 3u, operand)));

    auto swapped = builder.atomicCompareExchangeWeak(target, 3u, comparand, operand);
    results.add(wordOf(swapped.oldValue));
    results.add(select(swapped.exchanged, 1u, 0u));

    for (auto i = 0; i < results.size(); ++i)
        builder.write(output, (unsigned) i, results[i]);
}

struct BufferKernel
{
    explicit BufferKernel(bool isSigned)
    {
        output = builder.uintOutputBuffer();
        builder.threadId();

        if (isSigned)
            recordEveryOperation(builder, builder.intAtomicBuffer(), output, -5, 7);
        else
            recordEveryOperation(builder, builder.atomicBuffer(), output, 5u, 7u);
    }

    ShaderBuilder builder;
    UIntOutputBuffer output;
};

template <typename T>
struct SharedKernel
{
    SharedKernel()
    {
        output = builder.uintOutputBuffer();
        auto lane = builder.localId();
        auto counters = builder.sharedAtomic<T>(64);

        if constexpr (std::same_as<T, Int>)
        {
            builder.write(counters, lane, 0);
            builder.barrier();
            recordEveryOperation(builder, counters, output, -5, 7);
        }
        else
        {
            builder.write(counters, lane, 0u);
            builder.barrier();
            recordEveryOperation(builder, counters, output, 5u, 7u);
        }
    }

    ShaderBuilder builder;
    UIntOutputBuffer output;
};

struct Spelling
{
    std::string_view element;
    std::string_view type;
    std::string_view operand;
    std::string_view negated;
    std::string_view comparand;
};

constexpr auto metalFunctions = std::array {"atomic_fetch_add_explicit",
                                            "atomic_fetch_sub_explicit",
                                            "atomic_fetch_max_explicit",
                                            "atomic_fetch_min_explicit",
                                            "atomic_fetch_and_explicit",
                                            "atomic_fetch_or_explicit",
                                            "atomic_fetch_xor_explicit",
                                            "atomic_exchange_explicit"};

constexpr auto hlslFunctions = std::array {"InterlockedAdd",
                                           "InterlockedAdd",
                                           "InterlockedMax",
                                           "InterlockedMin",
                                           "InterlockedAnd",
                                           "InterlockedOr",
                                           "InterlockedXor",
                                           "InterlockedExchange"};

constexpr auto glslFunctions = std::array {"atomicAdd",
                                           "atomicAdd",
                                           "atomicMax",
                                           "atomicMin",
                                           "atomicAnd",
                                           "atomicOr",
                                           "atomicXor",
                                           "atomicExchange"};

constexpr auto subtraction = 1;

std::string_view operandAt(const Spelling& spelling, int which, bool negates)
{
    return negates && which == subtraction ? spelling.negated : spelling.operand;
}

// What recordEveryOperation's ten statements print as, in each dialect.
void expectEveryOperation(
    const Sources& sources,
    const Spelling& spelling,
    const std::source_location& where = std::source_location::current())
{
    for (auto which = 0; which < (int) metalFunctions.size(); ++which)
    {
        auto at = static_cast<std::size_t>(which);

        check(has(sources.metal,
                  Strings::format("{} v{} = {}(&{}, {}, memory_order_relaxed);",
                                  spelling.type,
                                  which,
                                  metalFunctions[at],
                                  spelling.element,
                                  spelling.operand)),
              metalFunctions[at],
              where);

        check(has(sources.hlsl,
                  Strings::format("{} v{};\n    {}({}, {}, v{});",
                                  spelling.type,
                                  which,
                                  hlslFunctions[at],
                                  spelling.element,
                                  operandAt(spelling, which, true),
                                  which)),
              hlslFunctions[at],
              where);

        check(has(sources.glsl,
                  Strings::format("{} v{} = {}({}, {});",
                                  spelling.type,
                                  which,
                                  glslFunctions[at],
                                  spelling.element,
                                  operandAt(spelling, which, true))),
              glslFunctions[at],
              where);
    }

    check(has(sources.metal,
              Strings::format("{} v8 = {};\n    bool v9 = "
                              "atomic_compare_exchange_weak_explicit(&{}, &v8, {}, "
                              "memory_order_relaxed, memory_order_relaxed);",
                              spelling.type,
                              spelling.comparand,
                              spelling.element,
                              spelling.operand)),
          "metal compare-exchange",
          where);

    check(has(sources.hlsl,
              Strings::format("{} cx8 = {};\n    {} v8;\n    "
                              "InterlockedCompareExchange({}, cx8, {}, v8);\n    "
                              "bool v9 = v8 == cx8;",
                              spelling.type,
                              spelling.comparand,
                              spelling.type,
                              spelling.element,
                              spelling.operand)),
          "hlsl compare-exchange",
          where);

    check(has(sources.glsl,
              Strings::format("{} cx8 = {};\n    {} v8 = atomicCompSwap({}, cx8, "
                              "{});\n    bool v9 = v8 == cx8;",
                              spelling.type,
                              spelling.comparand,
                              spelling.type,
                              spelling.element,
                              spelling.operand)),
          "glsl compare-exchange",
          where);
}

constexpr auto unsignedBuffer =
    Spelling {"buffer1[3u]", "uint", "5u", "0u - (5u)", "7u"};
constexpr auto signedBuffer = Spelling {"buffer1[3u]", "int", "-5", "-(-5)", "7"};
constexpr auto unsignedShared = Spelling {"s0[3u]", "uint", "5u", "0u - (5u)", "7u"};
constexpr auto signedShared = Spelling {"s0[3u]", "int", "-5", "-(-5)", "7"};
} // namespace

auto tUIntBuffer =
    test("AtomicOpsCodegen/everyOperationOnAnAtomicBufferIsSpelled") = []
{
    auto kernel = BufferKernel {false};
    auto sources = Sources {kernel.builder.graph()};

    check(has(sources.metal, "device atomic_uint* buffer1 [[buffer(1)]]"));
    check(has(sources.hlsl, "RWStructuredBuffer<uint> buffer1 : register(u1);"));
    check(has(sources.glsl, "buffer Buffer1\n{\n    uint buffer1[];\n};"));

    expectEveryOperation(sources, unsignedBuffer);
    expectGlslCompiles(kernel.builder.graph());
};

auto tIntBuffer =
    test("AtomicOpsCodegen/everyOperationOnAnIntAtomicBufferIsSpelledSigned") = []
{
    auto kernel = BufferKernel {true};
    auto sources = Sources {kernel.builder.graph()};

    check(has(sources.metal, "device atomic_int* buffer1 [[buffer(1)]]"));
    check(has(sources.hlsl, "RWStructuredBuffer<int> buffer1 : register(u1);"));
    check(has(sources.glsl, "buffer Buffer1\n{\n    int buffer1[];\n};"));

    expectEveryOperation(sources, signedBuffer);
    expectGlslCompiles(kernel.builder.graph());
};

auto tUIntShared =
    test("AtomicOpsCodegen/everyOperationOnASharedAtomicIsSpelled") = []
{
    auto kernel = SharedKernel<UInt> {};
    auto sources = Sources {kernel.builder.graph()};

    check(has(sources.metal, "    threadgroup atomic_uint s0[64];\n"));
    check(has(sources.hlsl, "groupshared uint s0[64];\n"));
    check(has(sources.glsl, "shared uint s0[64];\n"));

    check(has(sources.metal,
              "atomic_store_explicit(&s0[lid], 0u, memory_order_relaxed);"));
    check(has(sources.hlsl, "s0[lid] = 0u;"));
    check(has(sources.glsl, "s0[lid] = 0u;"));

    expectEveryOperation(sources, unsignedShared);
    expectGlslCompiles(kernel.builder.graph());
};

auto tIntShared =
    test("AtomicOpsCodegen/everyOperationOnASignedSharedAtomicIsSpelledSigned") = []
{
    auto kernel = SharedKernel<Int> {};
    auto sources = Sources {kernel.builder.graph()};

    check(has(sources.metal, "    threadgroup atomic_int s0[64];\n"));
    check(has(sources.hlsl, "groupshared int s0[64];\n"));
    check(has(sources.glsl, "shared int s0[64];\n"));

    expectEveryOperation(sources, signedShared);
    expectGlslCompiles(kernel.builder.graph());
};

// The commonest use of a compare-exchange: the comparand is a load of the very
// element it is compared against. HLSL and GLSL name it before the call, and
// Metal starts the old value from it, so no dialect reads the element again.
auto tCasLoop =
    test("AtomicOpsCodegen/aCompareExchangeLoopReadsItsComparandFirst") = []
{
    auto builder = ShaderBuilder {};
    auto counter = builder.atomicBuffer();
    auto tickets = builder.uintOutputBuffer();
    auto id = builder.threadId();
    auto done = builder.var(false);
    auto ticket = builder.var(0u);

    auto attempt = [&]
    {
        auto seen = counter.load(0u);
        auto outcome =
            builder.atomicCompareExchangeWeak(counter, 0u, seen, seen + 1u);

        auto take = [&]
        {
            ticket = outcome.oldValue;
            done = true;
        };

        builder.ifThen(outcome.exchanged, take);
    };

    builder.loop(!done.get(), attempt);
    builder.write(tickets, id, ticket.get());

    auto sources = Sources {builder.graph()};

    check(
        has(sources.metal,
            "uint t0 = atomic_load_explicit(&buffer0[0u], memory_order_relaxed);"));
    check(has(sources.metal, "uint v2 = t0;"));
    check(has(sources.metal,
              "atomic_compare_exchange_weak_explicit(&buffer0[0u], "
              "&v2, (t0 + 1u),"));

    check(has(sources.hlsl, "uint t0 = buffer0[0u];"));
    check(has(sources.hlsl, "uint cx2 = t0;"));
    check(has(sources.hlsl,
              "InterlockedCompareExchange(buffer0[0u], cx2, (t0 + 1u), v2);"));

    check(has(sources.glsl, "uint cx2 = t0;"));
    check(
        has(sources.glsl, "uint v2 = atomicCompSwap(buffer0[0u], cx2, (t0 + 1u));"));

    expectGlslCompiles(builder.graph());
};

// An atomic on shared memory moves it, as a store does: a name that held a
// load of it before is given up, and the read after loads again.
auto tSharedStaleness =
    test("AtomicOpsCodegen/aSharedLoadIsNotCarriedAcrossASharedAtomic") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.uintOutputBuffer();
    auto lane = builder.localId();
    auto counters = builder.sharedAtomic<UInt>(4);

    builder.write(counters, lane, 0u);
    builder.barrier();

    auto seen = counters.load(0u);
    builder.write(output, lane * 2u, seen);
    builder.write(output, lane * 2u + 1u, seen + 1u);
    builder.atomicAdd(counters, 0u, 1u);
    builder.write(output, lane + 128u, seen);

    auto sources = Sources {builder.graph()};

    check(occurrences(sources.metal,
                      "atomic_load_explicit(&s0[0u], memory_order_relaxed)")
          == 2);
    check(occurrences(sources.hlsl, "s0[0u]") == 3);
    check(occurrences(sources.glsl, "s0[0u]") == 3);
    expectGlslCompiles(builder.graph());
};

// The one int literal with no positive twin is printed so GLSL takes it.
auto tIntMinLiteral = test("AtomicOpsCodegen/theSmallestIntLiteralCompiles") = []
{
    auto builder = ShaderBuilder {};
    auto extremes = builder.intAtomicBuffer();
    builder.threadId();
    builder.write(extremes, 0u, INT_MIN);
    builder.atomicMax(extremes, 1u, INT_MIN);

    auto sources = Sources {builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        check(has(*source, "(-2147483647 - 1)"));
        check(!has(*source, "-2147483648"));
    }

    expectGlslCompiles(builder.graph());
};
} // namespace AtomicOpsCodegenTests
