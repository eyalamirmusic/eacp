#include "CountingHeap.h"

#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>
#include <eacp/GPU/CpuCompute/Helpers.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

// WGSL's structs on the CPU executor: a buffer of them read and written a
// field at a time at StructLayout's offsets - three-vectors, matrices, f16
// members and nested structs among them - with the bytes between fields left
// as they were; an element written whole taking every field's value from
// before any lands; struct variables and private arrays of them; elements past
// the binding; the refusal of any binding but the struct's own; and the
// realtime contract.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace StructTests
{
namespace
{
constexpr auto threadCount = 37;
constexpr auto sentinelByte = std::byte {0xab};

const auto batchings = std::array {PlanOptions {}, PlanOptions {1}};

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

static_assert(StructLayout<Particle>::size == 32);
static_assert(StructLayout<Outer>::size == 112);
static_assert(StructLayout<Halves>::stride == 2);

struct MoveKernel final : ComputeKernel
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

// An element rewritten whole from itself, `a` and inner.xy.x trading places,
// which only works if every field is taken before any lands; then its nested
// member rewritten from what the whole write left - a read of an output is
// what the thread stored there by the statement that reads it.
struct SwapKernel final : ComputeKernel
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
        write(halves, id, Halves {o.inner.h + 0.25f});
    }

    Uniform<StructOutputBuffer<Outer>> values;
    Uniform<StructOutputBuffer<Halves>> halves;
    EACP_SHADER(values, halves)
};

struct VarsKernel final : ComputeKernel
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

struct OutOfRangeKernel final : ComputeKernel
{
    OutOfRangeKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto p = input[id + 4u];
        write(output,
              id + 2u,
              Particle {p.position, p.mass + 1.f, p.velocity, p.flags});
    }

    Uniform<StructInputBuffer<Particle>> input;
    Uniform<StructOutputBuffer<Particle>> output;
    EACP_SHADER(input, output)
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

template <typename T>
void fillBytes(StructBufferData<T>& data, std::byte value)
{
    for (auto& byte: data.bytes())
        byte = value;
}

template <typename T>
bool keepsBytes(const StructBufferData<T>& data, int element, int from, int to)
{
    auto bytes = data.bytes();

    for (auto at = from; at < to; ++at)
        if (bytes[(std::size_t) (element * StructLayout<T>::stride + at)]
            != sentinelByte)
            return false;

    return true;
}

StructBufferData<Outer> outers(int count)
{
    auto data = StructBufferData<Outer> {count};
    fillBytes(data, sentinelByte);

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

auto tLayout = test("Struct/theLayoutIsWgslStorageLayout") = []
{
    check(StructLayout<Particle>::offsetOf<&Particle::mass>() == 12);
    check(StructLayout<Particle>::offsetOf<&Particle::velocity>() == 16);
    check(StructLayout<Outer>::offsetOf<&Outer::hm>() == 44);
    check(StructLayout<Outer>::offsetOf<&Outer::m>() == 64);
    check(StructLayout<Outer>::alignment == 16);
    check(!StructLayout<Flagged>::hostShareable);
};

auto tMove = test("Struct/aBufferOfStructsIsReadAndWrittenAtItsLayout") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = MoveKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = particles(threadCount);
        auto output = StructBufferData<Particle> {threadCount};
        fillBytes(output, sentinelByte);

        auto bindings = Bindings {};
        check(
            bindings.set(kernel.input, std::span<const std::byte> {input.bytes()}));
        check(bindings.set(kernel.output, output.bytes()));
        check(executor.dispatch(bindings, threadCount));

        for (auto i = 0; i < threadCount; ++i)
        {
            auto f = (float) i;
            auto position = output.get<&Particle::position>(i);
            auto velocity = output.get<&Particle::velocity>(i);

            check(position[0] == f + 0.5f && position[1] == f * 0.5f - f
                  && position[2] == -f + 1.f);
            check(output.get<&Particle::mass>(i) == (1.f + f * 0.25f) * 0.5f + 1.f);
            check(velocity[0] == -f && velocity[1] == 0.5f);
            check(output.get<&Particle::flags>(i) == ((std::uint32_t) i * 2u | 1u));
            check(keepsBytes(output, i, 28, 32), "the padding is never written");
        }
    }
};

auto tNested = test("Struct/nestedStructsMatricesAndHalvesKeepTheirOffsets") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = SwapKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto values = outers(threadCount);
        auto halves = StructBufferData<Halves> {threadCount};

        auto bindings = Bindings {};
        check(bindings.set(kernel.values, values.bytes()));
        check(bindings.set(kernel.halves, halves.bytes()));
        check(executor.dispatch(bindings, threadCount));

        for (auto i = 0; i < threadCount; ++i)
        {
            auto f = (float) i;
            auto xy = values.get<&Outer::inner, &Inner::xy>(i);
            auto hm = values.get<&Outer::hm>(i);
            auto m = values.get<&Outer::m>(i);
            auto cell = values.get<&Outer::cell>(i);
            auto v = values.get<&Outer::v>(i);

            check(values.get<&Outer::a>(i) == f + 0.5f,
                  "a took inner.xy.x from before the whole write");
            check(xy[0] == -f && xy[1] == f, "inner.xy.x took a from before it");
            check(values.get<&Outer::inner, &Inner::h>(i) == quantizeToF16(0.5f * f),
                  "the inner write read the half the whole one stored");
            check(v[0] == 2.f && v[1] == 2.f * f && v[2] == 4.f);
            check(hm[0] == 0.5f && hm[1] == 1.f && hm[2] == quantizeToF16(0.5f * f)
                  && hm[3] == -2.f);
            check(m[0] == 1.f && m[3] == f && m[5] == 6.f);
            check(cell[0] == i - 3 && cell[1] == -4 && cell[2] == 4);
            check(halves.get<&Halves::value>(i)
                  == quantizeToF16(quantizeToF16(0.5f * f) + 0.25f));

            check(keepsBytes(values, i, 4, 8) && keepsBytes(values, i, 18, 32)
                      && keepsBytes(values, i, 52, 64)
                      && keepsBytes(values, i, 108, 112),
                  "every gap between fields keeps its bytes");
        }
    }
};

auto tHalves = test("Struct/aStructOfHalvesAloneIsTwoBytesAnElement") = []
{
    auto kernel = SwapKernel {};
    auto executor = Executor {kernel};
    auto values = outers(5);
    auto halves = StructBufferData<Halves> {6};
    fillBytes(halves, sentinelByte);

    auto bindings = Bindings {};
    bindings.set(kernel.values, values.bytes());
    bindings.set(kernel.halves, halves.bytes());
    check(executor.dispatch(bindings, 5));

    check(halves.bytes().size() == 12);

    for (auto i = 0; i < 5; ++i)
        check(halves.get<&Halves::value>(i)
              == quantizeToF16(quantizeToF16(0.5f * (float) i) + 0.25f));

    check(keepsBytes(halves, 5, 0, 2), "the element past the dispatch is left");
};

auto tVars = test("Struct/variablesAndPrivateArraysHoldStructs") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = VarsKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = particles(threadCount);
        auto output = Vector<float> {};
        output.resize(threadCount, -1.f);

        auto bindings = Bindings {};
        bindings.set(kernel.input, std::span<const std::byte> {input.bytes()});
        bindings.set(kernel.output,
                     std::span<float> {output.data(), (std::size_t) output.size()});
        check(executor.dispatch(bindings, threadCount));

        for (auto i = 0; i < threadCount; ++i)
        {
            auto f = (float) i;
            auto mass = 1.f + f * 0.25f;
            auto slot = i & 3;
            auto chosenMass = slot == 1 ? mass * 2.f : 0.f;
            auto chosenY = slot == std::min(i, 3) ? 0.5f : (slot == 1 ? -f : 0.f);
            auto flags = (float) (i * 2);
            auto expected = i < 3 ? chosenMass + (mass + 1.f) : chosenY + flags;

            check(output[i] == expected,
                  "lane " + std::to_string(i) + ": " + std::to_string(output[i])
                      + " against " + std::to_string(expected));
        }
    }
};

auto tOutOfRange =
    test("Struct/anElementPastTheBindingReadsZeroAndIsNotWritten") = []
{
    auto kernel = OutOfRangeKernel {};
    auto executor = Executor {kernel};
    auto input = particles(6);
    auto output = StructBufferData<Particle> {5};
    fillBytes(output, sentinelByte);

    auto bindings = Bindings {};
    bindings.set(kernel.input, std::span<const std::byte> {input.bytes()});
    bindings.set(kernel.output, output.bytes());
    check(executor.dispatch(bindings, 4));

    check(output.get<&Particle::mass>(2) == 1.f + 4.f * 0.25f + 1.f);
    check(output.get<&Particle::mass>(3) == 1.f + 5.f * 0.25f + 1.f);
    check(output.get<&Particle::mass>(4) == 1.f, "element 8 reads zero");
    check(output.get<&Particle::flags>(4) == 0u);
    check(keepsBytes(output, 0, 0, 32) && keepsBytes(output, 1, 0, 32),
          "elements nothing stored to keep their bytes");
};

auto tRefusal = test("Struct/onlyTheStructsOwnBindingIsTaken") = []
{
    auto kernel = MoveKernel {};
    auto executor = Executor {kernel};
    auto input = particles(4);
    auto output = StructBufferData<Particle> {4};
    auto floats = Vector<float> {};
    floats.resize(32, 0.f);

    auto wrongStruct = StructInputBuffer<Halves> {};
    wrongStruct.slot = kernel.input.slot;

    auto asHalves = Bindings {};
    asHalves.set(wrongStruct, std::span<const std::byte> {input.bytes()});
    asHalves.set(kernel.output, output.bytes());
    check(!executor.dispatch(asHalves, 4));

    auto plain = InputBuffer {};
    plain.slot = kernel.input.slot;

    auto asFloats = Bindings {};
    asFloats.set(
        plain, std::span<const float> {floats.data(), (std::size_t) floats.size()});
    asFloats.set(kernel.output, output.bytes());
    check(!executor.dispatch(asFloats, 4));

    struct HostParticle
    {
        float position[3];
        float mass;
        float velocity[2];
        std::uint32_t flags;
        std::uint32_t padding;
    };

    auto records = std::array<HostParticle, 4> {};
    records[2].mass = 3.f;

    auto asRecords = Bindings {};
    check(asRecords.set(kernel.input, std::span<const HostParticle> {records}));
    asRecords.set(kernel.output, output.bytes());
    check(executor.dispatch(asRecords, 4));
    check(output.get<&Particle::mass>(2) == 3.f * 0.5f + 1.f);
};

auto tNoAllocation = test("Struct/aDispatchAfterTheFirstAllocatesNothing") = []
{
    auto move = MoveKernel {};
    auto swap = SwapKernel {};
    auto vars = VarsKernel {};
    auto moveExecutor = Executor {move};
    auto swapExecutor = Executor {swap};
    auto varsExecutor = Executor {vars};

    auto input = particles(threadCount);
    auto moved = StructBufferData<Particle> {threadCount};
    auto values = outers(threadCount);
    auto halves = StructBufferData<Halves> {threadCount};
    auto output = Vector<float> {};
    output.resize(threadCount, 0.f);

    auto moveBindings = Bindings {};
    moveBindings.set(move.input, std::span<const std::byte> {input.bytes()});
    moveBindings.set(move.output, moved.bytes());

    auto swapBindings = Bindings {};
    swapBindings.set(swap.values, values.bytes());
    swapBindings.set(swap.halves, halves.bytes());

    auto varsBindings = Bindings {};
    varsBindings.set(vars.input, std::span<const std::byte> {input.bytes()});
    varsBindings.set(vars.output,
                     std::span<float> {output.data(), (std::size_t) output.size()});

    auto dispatchAll = [&]
    {
        return moveExecutor.dispatch(moveBindings, threadCount)
               && swapExecutor.dispatch(swapBindings, threadCount)
               && varsExecutor.dispatch(varsBindings, threadCount);
    };

    auto warmedUp = dispatchAll();

    ExecutorTests::allocationsCounted = 0;
    ExecutorTests::countingHeap = true;
    auto ran = dispatchAll();
    ExecutorTests::countingHeap = false;

    check(warmedUp);
    check(ran);
    check(ExecutorTests::allocationsCounted.load() == 0);
};
} // namespace
} // namespace StructTests
