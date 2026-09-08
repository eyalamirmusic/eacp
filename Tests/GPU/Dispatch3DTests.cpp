#include "Common.h"

#include <string>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// The value element (x, y, z) carries, chosen so a swapped pair of extents, a
// wrong stride and a thread that ran twice are all different numbers. Every
// one of them is exact in float.
float expectedAt(int x, int y, int z)
{
    return (float) x + (float) y * 1000.f + (float) z * 1000000.f;
}

constexpr auto sentinel = -1.f;

// Elements past the volume, kept as sentinel: a dispatch rounds up to whole
// groups on every axis, so an unguarded thread writes here.
constexpr auto padding = 64;

struct VolumeKernel final : ComputeProgram
{
    VolumeKernel() { compile(); }

    void define() override
    {
        auto p = threadPosition3();
        auto index = (p.z * height + p.y) * width + p.x;

        write(output,
              index,
              toFloat(p.x) + toFloat(p.y) * 1000.f + toFloat(p.z) * 1000000.f);
    }

    Uniform<OutputBuffer> output;
    Uniform<UInt> width;
    Uniform<UInt> height;

    EACP_SHADER(output, width, height)
};

// Rebuilds the global position out of the group and local ones, and reports
// the three extents beside it, so both halves are checked against the very
// dispatch that produced them.
struct GroupIdVolumeKernel final : ComputeProgram
{
    GroupIdVolumeKernel() { compile(); }

    void define() override
    {
        auto p = threadPosition3();
        auto group = groupPosition3();
        auto local = localPosition3();
        auto size = (unsigned) groupSize3D;

        auto index = (p.z * gridHeight() + p.y) * gridWidth() + p.x;

        write(ids,
              index,
              toFloat(group.x * size + local.x)
                  + toFloat(group.y * size + local.y) * 1000.f
                  + toFloat(group.z * size + local.z) * 1000000.f);

        write(extents,
              index,
              float3(toFloat(gridWidth()),
                     toFloat(gridHeight()),
                     toFloat(gridDepth())));
    }

    Uniform<OutputBuffer> ids;
    Uniform<OutputBuffer> extents;

    EACP_SHADER(ids, extents)
};

std::vector<float> filledWithSentinel(int count)
{
    return std::vector<float>((std::size_t) count, sentinel);
}

void checkVolumeFill(int width, int height, int depth)
{
    auto& device = Device::shared();
    auto cells = width * height * depth;
    auto initial = filledWithSentinel(cells + padding);
    auto bytes = (int) (initial.size() * sizeof(float));

    auto output = device.makeBuffer(initial.data(), bytes, BufferUsage::Storage);

    auto kernel = VolumeKernel {};
    kernel.output = output;
    kernel.width = (std::uint32_t) width;
    kernel.height = (std::uint32_t) height;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, width, height, depth);
        }

        commands.commit();
    }

    auto result = filledWithSentinel(cells + padding);
    output.read(result.data(), bytes);

    for (auto z = 0; z < depth; ++z)
        for (auto y = 0; y < height; ++y)
            for (auto x = 0; x < width; ++x)
                check(result[(std::size_t) ((z * height + y) * width + x)]
                      == expectedAt(x, y, z));

    for (auto i = cells; i < cells + padding; ++i)
        check(result[(std::size_t) i] == sentinel);
}
} // namespace

// The emitted text for a kernel over a volume: a three-component id in the
// signature, three extents in the uniform block, a guard over all three, and a
// threadgroup that is the same 64 threads the other two ranks budget for.
auto tCodegenCompute3D = test("GPU/codegenCompute3D") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto p = builder.threadPosition3();

    builder.write(output, (p.z * 16u + p.y) * 16u + p.x, toFloat(p.z));

    auto shader = builder.build();
    check(shader.dispatchRank == DispatchRank::ThreeD);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint3 gid [[thread_position_in_grid]]"));
    check(contains(metal, "uint width;"));
    check(contains(metal, "uint height;"));
    check(contains(metal, "uint depth;"));
    check(!contains(metal, "uint count;"));
    check(contains(metal,
                   "if (gid.x >= uniforms.width || gid.y >= uniforms.height || "
                   "gid.z >= uniforms.depth)"));
    check(contains(metal, "((gid.z * 16u) + gid.y)"));
    check(contains(metal, "= float(gid.z);"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "[numthreads(4, 4, 4)]"));
    check(contains(hlsl, "uint3 threadId : SV_DispatchThreadID"));
    check(contains(hlsl, "uint3 gid = threadId.xyz;"));
    check(contains(hlsl,
                   "if (gid.x >= uniforms.width || gid.y >= uniforms.height || "
                   "gid.z >= uniforms.depth)"));
    check(contains(hlsl, "((gid.z * 16u) + gid.y)"));
    check(contains(hlsl, "= float(gid.z);"));

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "layout(local_size_x = 4, local_size_y = 4, local_size_z = 4)"));
    check(contains(glsl, "uvec3 gid = gl_GlobalInvocationID.xyz;"));
    check(contains(glsl,
                   "if (gid.x >= uniforms.width || gid.y >= uniforms.height || "
                   "gid.z >= uniforms.depth)"));
    check(contains(glsl, "((gid.z * 16u) + gid.y)"));
    check(contains(glsl, "= float(gid.z);"));

    expectGlslCompiles(builder.graph());
};

// The threadgroup vocabulary of a 3D kernel, in the entry signature of both
// backends: three components everywhere, as the global id has.
auto tCodegenCompute3DGroupIds = test("GPU/codegenCompute3DGroupIds") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto p = builder.threadPosition3();
    auto local = builder.localPosition3();
    auto group = builder.groupPosition3();

    builder.write(output, p.x, toFloat(local.z + group.z + builder.gridDepth()));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint3 lid [[thread_position_in_threadgroup]]"));
    check(contains(metal, "uint3 tgid [[threadgroup_position_in_grid]]"));
    check(contains(metal, "lid.z"));
    check(contains(metal, "tgid.z"));
    check(contains(metal, "uniforms.depth"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "uint3 localThread : SV_GroupThreadID"));
    check(contains(hlsl, "uint3 groupIndex : SV_GroupID"));
    check(contains(hlsl, "uint3 lid = localThread.xyz;"));
    check(contains(hlsl, "uint3 tgid = groupIndex.xyz;"));

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "uvec3 lid = gl_LocalInvocationID.xyz;"));
    check(contains(glsl, "uvec3 tgid = gl_WorkGroupID.xyz;"));
    check(contains(glsl, "uniforms.depth"));

    expectGlslCompiles(builder.graph());
};

// Every cell of a volume runs exactly once and nothing outside it writes at
// all. The extents are deliberately not multiples of the 4x4x4 group, and the
// buffer is longer than the volume so an unguarded thread has somewhere
// visible to land.
auto tVolumeDispatchCoversTheVolume =
    test("Dispatch3D/volumeDispatchCoversTheVolume") = []
{
    if (!Device::shared().isValid())
        return;

    checkVolumeFill(5, 6, 7);
};

// The same, with one axis of 1 in turn: a rank is not a shape, so a flat
// volume is still dispatched as one.
auto tVolumeDispatchWithFlatAxis = test("Dispatch3D/volumeDispatchWithFlatAxis") = []
{
    if (!Device::shared().isValid())
        return;

    checkVolumeFill(5, 6, 1);
    checkVolumeFill(5, 1, 7);
    checkVolumeFill(1, 6, 7);
};

// groupPosition3 * groupSize3D + localPosition3 is threadPosition3, on all
// three axes, and gridWidth/gridHeight/gridDepth are what the dispatch was
// given.
auto tGroupAndLocalPositionsAgree =
    test("Dispatch3D/groupAndLocalPositionsAgree") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto width = 5;
    constexpr auto height = 6;
    constexpr auto depth = 7;
    constexpr auto cells = width * height * depth;

    auto ids = device.makeBuffer((int) sizeof(float) * cells, BufferUsage::Storage);
    auto extents =
        device.makeBuffer((int) sizeof(float) * cells * 3, BufferUsage::Storage);

    auto kernel = GroupIdVolumeKernel {};
    kernel.ids = ids;
    kernel.extents = extents;
    kernel.prepare();

    {
        auto commands = device.makeCommandBuffer();

        {
            auto pass = commands.beginCompute();
            pass.dispatch(kernel, width, height, depth);
        }

        commands.commit();
    }

    auto fromIds = std::vector<float>((std::size_t) cells);
    auto fromExtents = std::vector<float>((std::size_t) cells * 3);
    ids.read(fromIds.data(), (int) (fromIds.size() * sizeof(float)));
    extents.read(fromExtents.data(), (int) (fromExtents.size() * sizeof(float)));

    for (auto z = 0; z < depth; ++z)
    {
        for (auto y = 0; y < height; ++y)
        {
            for (auto x = 0; x < width; ++x)
            {
                auto cell = (std::size_t) ((z * height + y) * width + x);

                check(fromIds[cell] == expectedAt(x, y, z));
                check(fromExtents[cell * 3] == (float) width);
                check(fromExtents[cell * 3 + 1] == (float) height);
                check(fromExtents[cell * 3 + 2] == (float) depth);
            }
        }
    }
};
