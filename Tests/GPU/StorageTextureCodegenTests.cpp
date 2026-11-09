#include "CodegenCommon.h"

#include <eacp/GPU/Codegen/StorageTextures.h>

#include <initializer_list>
#include <string>

// WGSL's storage textures as text in all three dialects: each access mode on
// each shape, float, unsigned and signed formats, loads, stores and queries,
// and a fragment shader reading one. Every GLSL source is compiled wherever
// eacp-spirv is built; what the hardware does is StorageTextureDeviceTests'.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace StorageTextureCodegenTests
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
        check(contains(source, text), text, location);
}

void checkNone(
    const std::string& source,
    std::initializer_list<const char*> unexpected,
    const std::source_location& location = std::source_location::current())
{
    for (const auto* text: unexpected)
        check(!contains(source, text), text, location);
}

using Heights = StorageTexture2D<TextureFormat::R32Float, StorageAccess::ReadWrite>;
using Ids = StorageTexture2D<TextureFormat::R32Uint, StorageAccess::Read>;
using Offsets = StorageTexture2D<TextureFormat::RG16Sint>;
using Colours = StorageTexture2DArray<TextureFormat::RGBA8Unorm>;
using Volume =
    StorageTexture3D<TextureFormat::RGBA16Float, StorageAccess::ReadWrite>;
using Counts =
    StorageTexture2DArray<TextureFormat::RGBA32Uint, StorageAccess::ReadWrite>;
using Field = StorageTexture3D<TextureFormat::RGBA32Sint, StorageAccess::Read>;
using Glow = StorageTexture2D<TextureFormat::RG11B10Float, StorageAccess::ReadWrite>;

static_assert(std::is_same_v<StorageTexelOf<TextureFormat::R32Float>, Float4>);
static_assert(std::is_same_v<StorageTexelOf<TextureFormat::RG16Sint>, Int4>);
static_assert(std::is_same_v<StorageTexelOf<TextureFormat::RGBA8Uint>, UInt4>);
static_assert(std::is_same_v<decltype(dimensions(std::declval<Volume>())), UInt3>);
static_assert(std::is_same_v<decltype(dimensions(std::declval<Colours>())), UInt2>);

// Seven slots, one of every access on every shape, each loaded where it can
// be and stored where it can be: a whole kernel, so the three sources are
// compiled as one.
struct EveryKind
{
    EveryKind()
    {
        auto p = toInt(builder.threadId2());
        auto h = load(heights, p);
        auto voxel = int3(p.x(), p.y(), layer);

        builder.write(heights, p, h * 0.5f);
        builder.write(offsets, p, int4(toInt(load(ids, p).x()), 1, 2, 3));
        builder.write(colours, p, layer, float4(h.x(), 0.f, 0.f, 1.f));
        builder.write(volume, voxel, load(volume, voxel) + 1.f);
        builder.write(counts, p, 2, load(counts, p, layer) + 1u);
        builder.write(offsets, p, load(field, voxel));
    }

    ShaderBuilder builder;
    Heights heights = builder.storageTexture<Heights>();
    Ids ids = builder.storageTexture<Ids>();
    Offsets offsets = builder.storageTexture<Offsets>();
    Colours colours = builder.storageTexture<Colours>();
    Volume volume = builder.storageTexture<Volume>();
    Counts counts = builder.storageTexture<Counts>();
    Field field = builder.storageTexture<Field>();
    Int layer = builder.uniform<Int>();
};
} // namespace

auto tDeclarations =
    test("StorageTextureCodegen/eachAccessShapeAndFormatDeclaresItsOwnType") = []
{
    auto kernel = EveryKind {};
    auto sources = Sources {kernel.builder.graph()};

    checkAll(sources.metal,
             {"texture2d<float, access::read_write> texture0 [[texture(0)]]",
              "texture2d<uint, access::read> texture1 [[texture(1)]]",
              "texture2d<int, access::write> texture2 [[texture(2)]]",
              "texture2d_array<float, access::write> texture3 [[texture(3)]]",
              "texture3d<float, access::read_write> texture4 [[texture(4)]]",
              "texture2d_array<uint, access::read_write> texture5 [[texture(5)]]",
              "texture3d<int, access::read> texture6 [[texture(6)]]"});

    checkNone(sources.metal, {"sampler"});

    checkAll(sources.hlsl,
             {"RWTexture2D<float> texture0 : register(u8);",
              "Texture2D<uint4> texture1 : register(t9);",
              "RWTexture2D<int2> texture2 : register(u10);",
              "RWTexture2DArray<float4> texture3 : register(u11);",
              "RWTexture3D<float4> texture4 : register(u12);",
              "RWTexture2DArray<uint4> texture5 : register(u13);",
              "Texture3D<int4> texture6 : register(t14);"});

    checkNone(sources.hlsl, {"SamplerState"});

    checkAll(sources.glsl,
             {"layout(set = 0, binding = 8, r32f) uniform image2D texture0;",
              "layout(set = 0, binding = 9) uniform usampler2D texture1;",
              "layout(set = 0, binding = 10) uniform writeonly iimage2D texture2;",
              "layout(set = 0, binding = 11) uniform writeonly image2DArray "
              "texture3;",
              "layout(set = 0, binding = 12, rgba16f) uniform image3D texture4;",
              "layout(set = 0, binding = 13, rgba32ui) uniform uimage2DArray "
              "texture5;",
              "layout(set = 0, binding = 14) uniform isampler3D texture6;"});

    expectGlslCompiles(kernel.builder.graph());
};

// HLSL's typed UAV is as wide as the format, so a narrower one stores a
// swizzle and loads widened back to four, (0, 0, 1) filling the rest.
auto tLoadsAndStores =
    test("StorageTextureCodegen/loadsAndStoresAddressTexelLayerAndVoxel") = []
{
    auto kernel = EveryKind {};
    auto sources = Sources {kernel.builder.graph()};

    checkAll(sources.metal,
             {"float4 t1 = texture0.read(uint2(t0));",
              "texture0.write((t1 * 0.5), uint2(t0));\n    texture0.fence();",
              "texture1.read(uint2(t0), uint(0))",
              "texture2.write(int4(",
              "texture3.write(float4((t1).x, 0.0, 0.0, 1.0), uint2(t0), "
              "uint(uniforms.u0));",
              "texture4.write((texture4.read(uint3(t2)) + 1.0), uint3(t2));\n"
              "    texture4.fence();",
              "texture5.write((texture5.read(uint2(t0), uint(uniforms.u0)) + 1u), "
              "uint2(t0), uint(2));",
              "texture2.write(texture6.read(uint3(t2), uint(0)), uint2(t0));"});

    checkNone(sources.metal,
              {"texture2.fence()", "texture3.fence()", "texture1.fence()"});

    checkAll(sources.hlsl,
             {"float4 t1 = float4(texture0[uint2(t0)], 0.0, 0.0, 1.0);",
              "texture0[uint2(t0)] = ((t1 * 0.5)).x;",
              "texture1.Load(int3(t0, 0))",
              "texture2[uint2(t0)] = (int4(",
              ")).xy;",
              "texture3[uint3(uint2(t0), uint(uniforms.u0))] = float4((t1).x, 0.0, "
              "0.0, 1.0);",
              "texture4[uint3(t2)] = (texture4[uint3(t2)] + 1.0);",
              "texture5[uint3(uint2(t0), uint(2))] = "
              "(texture5[uint3(uint2(t0), uint(uniforms.u0))] + 1u);",
              "texture2[uint2(t0)] = (texture6.Load(int4(t2, 0))).xy;"});

    checkAll(sources.glsl,
             {"vec4 t1 = imageLoad(texture0, t0);",
              "imageStore(texture0, t0, (t1 * 0.5));",
              "texelFetch(texture1, t0, 0)",
              "imageStore(texture3, ivec3(t0, uniforms.u0), vec4((t1).x, 0.0, "
              "0.0, 1.0));",
              "imageStore(texture4, t2, (imageLoad(texture4, t2) + 1.0));",
              "imageStore(texture5, ivec3(t0, 2), (imageLoad(texture5, ivec3(t0, "
              "uniforms.u0)) + 1u));",
              "imageStore(texture2, t0, texelFetch(texture6, t2, 0));"});
};

auto tIntegerWidening =
    test("StorageTextureCodegen/hlslWidensANarrowIntegerLoadWgslsWay") = []
{
    using Labels =
        StorageTexture2D<TextureFormat::R32Uint, StorageAccess::ReadWrite>;
    using Signs =
        StorageTexture2D<TextureFormat::RG32Sint, StorageAccess::ReadWrite>;

    auto builder = ShaderBuilder {};
    auto labels = builder.storageTexture<Labels>();
    auto signs = builder.storageTexture<Signs>();
    auto glow = builder.storageTexture<Glow>();
    auto p = builder.threadId2();

    builder.write(labels, p, load(labels, p) + 1u);
    builder.write(signs, p, load(signs, p) - 1);
    builder.write(glow, p, load(glow, p) * 2.f);

    auto sources = Sources {builder.graph()};

    checkAll(sources.hlsl,
             {"RWTexture2D<uint> texture0 : register(u8);",
              "RWTexture2D<int2> texture1 : register(u9);",
              "RWTexture2D<float3> texture2 : register(u10);",
              "uint4(texture0[gid], 0u, 0u, 1u)",
              "int4(texture1[gid], 0, 1)",
              "float4(texture2[gid], 1.0)",
              ").x;",
              ").xy;",
              ").xyz;"});

    checkAll(sources.metal,
             {"texture2d<uint, access::read_write> texture0",
              "texture2d<int, access::read_write> texture1",
              "texture2d<float, access::read_write> texture2",
              "texture0.read(gid)"});

    checkAll(sources.glsl,
             {"layout(set = 0, binding = 8, r32ui) uniform uimage2D texture0;",
              "layout(set = 0, binding = 9, rg32i) uniform iimage2D texture1;",
              "layout(set = 0, binding = 10, r11f_g11f_b10f) uniform image2D "
              "texture2;",
              "imageLoad(texture0, ivec2(gid))"});

    expectGlslCompiles(builder.graph());
};

// HLSL answers through out-parameters on a type of the slot's own, so the
// helper is one per queried slot; a typed UAV's GetDimensions has no level.
auto tQueries = test("StorageTextureCodegen/queriesOnEveryShape") = []
{
    using Plane = StorageTexture2D<TextureFormat::RGBA8Unorm, StorageAccess::Read>;

    auto builder = ShaderBuilder {};
    auto output = builder.uintOutputBuffer();
    auto heights = builder.storageTexture<Heights>();
    auto counts = builder.storageTexture<Counts>();
    auto volume = builder.storageTexture<Volume>();
    auto plane = builder.storageTexture<Plane>();
    auto field = builder.storageTexture<Field>();
    auto i = builder.threadId();

    builder.write(output, i, dimensions(heights).x());
    builder.write(output, i + 1u, dimensions(counts).y());
    builder.write(output, i + 2u, layerCount(counts));
    builder.write(output, i + 3u, dimensions(volume).z());
    builder.write(output, i + 4u, dimensions(plane).x());
    builder.write(output, i + 5u, dimensions(field).z());

    auto sources = Sources {builder.graph()};

    checkAll(sources.metal,
             {"uint2(texture0.get_width(), texture0.get_height())",
              "texture1.get_array_size()",
              "uint3(texture2.get_width(), texture2.get_height(), "
              "texture2.get_depth())",
              "uint2(texture3.get_width(), texture3.get_height())"});

    checkAll(sources.hlsl,
             {"uint3 eacpStorageSize0(RWTexture2D<float> t)\n{\n"
              "    uint width, height;\n"
              "    t.GetDimensions(width, height);\n"
              "    return uint3(width, height, 1u);\n}",
              "uint3 eacpStorageSize1(RWTexture2DArray<uint4> t)\n{\n"
              "    uint width, height, depth;\n"
              "    t.GetDimensions(width, height, depth);\n"
              "    return uint3(width, height, depth);\n}",
              "uint3 eacpStorageSize2(RWTexture3D<float4> t)",
              "uint3 eacpStorageSize3(Texture2D<float4> t)\n{\n"
              "    uint width, height, levels;\n"
              "    t.GetDimensions(0, width, height, levels);\n"
              "    return uint3(width, height, 1u);\n}",
              "uint3 eacpStorageSize4(Texture3D<int4> t)\n{\n"
              "    uint width, height, depth, levels;\n"
              "    t.GetDimensions(0, width, height, depth, levels);\n"
              "    return uint3(width, height, depth);\n}",
              "eacpStorageSize0(texture0).xy",
              "eacpStorageSize1(texture1).xy",
              "eacpStorageSize1(texture1).z",
              "eacpStorageSize2(texture2).xyz",
              "eacpStorageSize3(texture3).xy",
              "eacpStorageSize4(texture4).xyz"});

    checkNone(sources.hlsl, {"eacpTextureInfo"});

    checkAll(sources.glsl,
             {"uvec2(imageSize(texture0).xy)",
              "uvec2(imageSize(texture1).xy)",
              "uint(imageSize(texture1).z)",
              "uvec3(imageSize(texture2))",
              "uvec2(textureSize(texture3, 0).xy)",
              "uvec3(textureSize(texture4, 0))"});

    expectGlslCompiles(builder.graph());
};

// The kernel written before storage textures existed, character for character.
auto tWritableUnchanged =
    test("StorageTextureCodegen/aWritableTexture2DEmitsAsItAlwaysHas") = []
{
    auto builder = ShaderBuilder {};
    auto target = builder.writableTexture();
    auto p = builder.threadPosition();
    builder.write(target, p.x, p.y, float4(toFloat(p.x), 0.f, 0.f, 1.f));

    auto sources = Sources {builder.graph()};

    checkAll(sources.metal,
             {"texture2d<float, access::write> texture0 [[texture(0)]],",
              "texture0.write(float4(float(gid.x), 0.0, 0.0, 1.0), uint2(gid.x, "
              "gid.y));"});
    checkNone(sources.metal, {"fence"});

    checkAll(sources.hlsl,
             {"RWTexture2D<float4> texture0 : register(u8);",
              "texture0[uint2(gid.x, gid.y)] = float4(float(gid.x), 0.0, 0.0, "
              "1.0);"});

    checkAll(sources.glsl,
             {"layout(set = 0, binding = 8) uniform writeonly image2D texture0;",
              "imageStore(texture0, ivec2(gid.x, gid.y), vec4(float(gid.x), 0.0, "
              "0.0, 1.0));"});

    expectGlslCompiles(builder.graph());
};

// A storage slot takes its index from the counter sampled ones do, and
// declares no sampler beside it.
auto tMixedSlots =
    test("StorageTextureCodegen/sampledAndStorageSlotsShareOneCounter") = []
{
    auto builder = ShaderBuilder {};
    auto image = builder.texture();
    auto heights = builder.storageTexture<Heights>();
    auto p = builder.threadId2();
    builder.write(
        heights, p, sampleLevel(image, float2(builder.constant(0.5f), 0.5f), 0.f));

    auto sources = Sources {builder.graph()};

    check(heights.slot == 1);

    checkAll(sources.metal,
             {"texture2d<float> texture0 [[texture(0)]],\n    sampler sampler0 "
              "[[sampler(0)]],\n    texture2d<float, access::read_write> texture1 "
              "[[texture(1)]],\n    constant Uniforms&"});

    checkAll(sources.hlsl,
             {"Texture2D texture0 : register(t8);",
              "RWTexture2D<float> texture1 : register(u9);",
              "SamplerState samplerConfig0 : register(s0);"});

    expectGlslCompiles(builder.graph());
};

// A fragment shader reads a Read storage texture through the sampled binding
// with no sampler in it: WGSL's texture_storage_2d<f, read> in a fragment.
auto tFragmentRead =
    test("StorageTextureCodegen/aFragmentShaderLoadsAReadStorageTexture") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    builder.position(float4(position, 0.f, 1.f));

    auto ids = builder.storageTexture<Ids>();
    auto image = builder.texture();
    auto texel = toInt(builder.fragmentCoordinate().xy());
    builder.fragment(toFloat(load(ids, texel)) * (1.f / 255.f)
                     + sampleLevel(image, float2(builder.constant(0.f), 0.f), 0.f));

    auto sources = Sources {builder.graph()};

    checkAll(sources.metal,
             {"texture2d<uint, access::read> texture0 [[texture(0)]],\n    "
              "texture2d<float> texture1 [[texture(1)]],\n    sampler sampler1 "
              "[[sampler(1)]]",
              "texture0.read(uint2("});
    checkNone(sources.metal, {"sampler sampler0"});

    checkAll(sources.hlsl,
             {"Texture2D<uint4> texture0 : register(t0);",
              "Texture2D texture1 : register(t1);",
              "texture0.Load(int3("});

    checkAll(sources.glsl,
             {"layout(set = 0, binding = 8) uniform usampler2D texture0;",
              "texelFetch(texture0, "});

    expectGlslCompiles(builder.graph());
};

auto tCpuRefuses =
    test("StorageTextureCodegen/theCpuExecutorRefusesStorageTextures") = []
{
    auto builder = ShaderBuilder {};
    auto heights = builder.storageTexture<Heights>();
    auto p = toInt(builder.threadId2());
    builder.write(heights, p, load(heights, p) + 1.f);

    auto executor = CpuCompute::Executor {builder.graph()};
    check(!executor.isValid());
    check(contains(executor.reason(), "texture"));
};
} // namespace StorageTextureCodegenTests
