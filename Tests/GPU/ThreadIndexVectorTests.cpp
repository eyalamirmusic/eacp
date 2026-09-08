#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

#include <cstdint>
#include <string>

// The thread index taken as one value: threadId2() and threadId3(), with the
// local and group siblings, beside the component structs threadPosition() and
// threadPosition3() give.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto uintBytes = (int) sizeof(std::uint32_t);
constexpr auto sentinel = 0xdeadbeefu;

// Elements past the grid, kept as sentinel: a dispatch rounds up to whole
// groups, so an unguarded thread writes here.
constexpr auto padding = 64;

constexpr auto columns = 13;
constexpr auto rows = 11;
constexpr auto cells = columns * rows;

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

Buffer makeSentinelBuffer(int elements)
{
    auto values = Vector<std::uint32_t> {};
    values.assign(elements, sentinel);

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

std::uint32_t expectedAt(int x, int y, int z = 0)
{
    return (std::uint32_t) x + (std::uint32_t) y * 1000u
           + (std::uint32_t) z * 1000000u;
}

// The pair addresses the grid on its own: a swizzle picks the lane the index
// needs, and the weighting is arithmetic on the whole value.
struct GridPairKernel final : ComputeProgram
{
    GridPairKernel() { compile(); }

    void define() override
    {
        auto g = threadId2();
        auto weighted = g * 1000u;

        write(output, g.y() * gridWidth() + g.x(), g.x() + weighted.y());
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

struct VolumeTripleKernel final : ComputeProgram
{
    VolumeTripleKernel() { compile(); }

    void define() override
    {
        auto g = threadId3();
        auto weighted = g * 1000u;

        auto index = (g.z() * gridHeight() + g.y()) * gridWidth() + g.x();

        write(output, index, g.x() + weighted.y() + weighted.z() * 1000u);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

// The global pair rebuilt out of the group and local ones, reported beside the
// pair itself so the two are compared at every cell.
struct RebuiltPairKernel final : ComputeProgram
{
    RebuiltPairKernel() { compile(); }

    void define() override
    {
        auto g = threadId2();
        auto rebuilt = groupId2() * (unsigned) groupSize2D + localId2();

        auto record = (g.y() * gridWidth() + g.x()) * 2u;

        write(output, record, rebuilt.x());
        write(output, record + 1u, rebuilt.y());
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

// A 2D kernel's whole position prints as the bare gid both backends already
// declare, with a swizzle where one lane is wanted - never a reconstruction.
auto tThreadIdPairIsTheWholeGid = test("ThreadIndexVector/aPairIsTheWholeGid") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto g = builder.threadId2();
    auto weighted = g * 1000u;

    builder.write(output, g.y() * builder.gridWidth() + g.x(), weighted.y());

    auto shader = builder.build();
    check(shader.dispatchRank == DispatchRank::TwoD);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint2 gid [[thread_position_in_grid]]"));
    check(contains(metal, "(gid * 1000u)"));
    check(contains(metal, "(gid).x"));
    check(contains(metal, "(gid).y"));
    check(
        contains(metal, "if (gid.x >= uniforms.width || gid.y >= uniforms.height)"));
    check(!contains(metal, "uint2(gid"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "uint2 gid = threadId.xy;"));
    check(contains(hlsl, "(gid * 1000u)"));
    check(contains(hlsl, "(gid).x"));
    check(
        contains(hlsl, "if (gid.x >= uniforms.width || gid.y >= uniforms.height)"));
    check(!contains(hlsl, "uint2(gid"));
};

// The same over a volume, in the uint3 a 3D kernel's entry point declares.
auto tThreadIdTripleIsTheWholeGid =
    test("ThreadIndexVector/aTripleIsTheWholeGid") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto g = builder.threadId3();
    auto weighted = g * 1000u;

    builder.write(output, g.z() * 16u + g.x(), weighted.z());

    auto shader = builder.build();
    check(shader.dispatchRank == DispatchRank::ThreeD);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint3 gid [[thread_position_in_grid]]"));
    check(contains(metal, "(gid * 1000u)"));
    check(contains(metal, "(gid).z"));
    check(contains(metal,
                   "if (gid.x >= uniforms.width || gid.y >= uniforms.height || "
                   "gid.z >= uniforms.depth)"));
    check(!contains(metal, "uint3(gid"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "uint3 gid = threadId.xyz;"));
    check(contains(hlsl, "(gid * 1000u)"));
    check(contains(hlsl, "(gid).z"));
    check(!contains(hlsl, "uint3(gid"));
};

// The threadgroup siblings print as the lid and tgid the same scaffolding
// declares, whole where the whole value is used.
auto tGroupAndLocalPairsAreWhole =
    test("ThreadIndexVector/groupAndLocalPairsAreWhole") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto rebuilt = builder.groupId2() * 8u + builder.localId2();

    builder.write(output, rebuilt.y() * 16u + rebuilt.x(), rebuilt.x());

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint2 lid [[thread_position_in_threadgroup]]"));
    check(contains(metal, "uint2 tgid [[threadgroup_position_in_grid]]"));
    check(contains(metal, "((tgid * 8u) + lid)"));
    check(!contains(metal, "uint2(tgid"));
    check(!contains(metal, "uint2(lid"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "uint2 lid = localThread.xy;"));
    check(contains(hlsl, "uint2 tgid = groupIndex.xy;"));
    check(contains(hlsl, "((tgid * 8u) + lid)"));
};

auto tGroupAndLocalTriplesAreWhole =
    test("ThreadIndexVector/groupAndLocalTriplesAreWhole") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto rebuilt = builder.groupId3() * 4u + builder.localId3();

    builder.write(output, rebuilt.z(), rebuilt.x());

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint3 lid [[thread_position_in_threadgroup]]"));
    check(contains(metal, "uint3 tgid [[threadgroup_position_in_grid]]"));
    check(contains(metal, "((tgid * 4u) + lid)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "uint3 lid = localThread.xyz;"));
    check(contains(hlsl, "uint3 tgid = groupIndex.xyz;"));
    check(contains(hlsl, "((tgid * 4u) + lid)"));
};

// Every cell of a grid that is not a multiple of the 8x8 group runs exactly
// once, addressed through the pair, and nothing past it writes at all.
auto tPairAddressesTheGrid = test("ThreadIndexVector/aPairAddressesTheGrid") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeSentinelBuffer(cells + padding);

    auto kernel = GridPairKernel {};
    kernel.output = output;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, columns, rows);
        }

        commands.commit();
    }

    auto values = readUInts(output, cells + padding);

    for (auto y = 0; y < rows; ++y)
        for (auto x = 0; x < columns; ++x)
            check(values[y * columns + x] == expectedAt(x, y));

    for (auto i = cells; i < cells + padding; ++i)
        check(values[i] == sentinel);
};

auto tTripleAddressesTheVolume =
    test("ThreadIndexVector/aTripleAddressesTheVolume") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto width = 5;
    constexpr auto height = 6;
    constexpr auto depth = 7;
    constexpr auto volume = width * height * depth;

    auto output = makeSentinelBuffer(volume + padding);

    auto kernel = VolumeTripleKernel {};
    kernel.output = output;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, width, height, depth);
        }

        commands.commit();
    }

    auto values = readUInts(output, volume + padding);

    for (auto z = 0; z < depth; ++z)
        for (auto y = 0; y < height; ++y)
            for (auto x = 0; x < width; ++x)
                check(values[(z * height + y) * width + x] == expectedAt(x, y, z));

    for (auto i = volume; i < volume + padding; ++i)
        check(values[i] == sentinel);
};

// groupId2() * groupSize2D + localId2() is threadId2(), on both lanes.
auto tRebuiltPairIsTheThreadId =
    test("ThreadIndexVector/groupTimesSizePlusLocalIsThePair") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeSentinelBuffer(cells * 2);

    auto kernel = RebuiltPairKernel {};
    kernel.output = output;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, columns, rows);
        }

        commands.commit();
    }

    auto values = readUInts(output, cells * 2);

    for (auto y = 0; y < rows; ++y)
    {
        for (auto x = 0; x < columns; ++x)
        {
            auto record = (y * columns + x) * 2;

            check(values[record] == (std::uint32_t) x);
            check(values[record + 1] == (std::uint32_t) y);
        }
    }
};
