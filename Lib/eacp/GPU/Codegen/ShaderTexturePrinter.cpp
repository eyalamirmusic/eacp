#include "ShaderExpressionPrinter.h"
#include "ShaderDeclarations.h"
#include "ShaderHelpers.h"

#include <eacp/Core/Utils/Format.h>

namespace eacp::GPU::ShaderEmission
{
namespace
{
int textureArgumentsBefore(TextureKind kind)
{
    return isLayered(kind) ? 2 : 1;
}

int textureExtra(const ShaderGraph& graph, const Expr& expr, int which)
{
    return expr.args[textureArgumentsBefore(graph.textureKind(expr.index)) + which];
}

std::string textureNameOf(const Expr& expr)
{
    return Strings::format("texture{}", expr.index);
}

std::string metalSamplerOf(const Expr& expr)
{
    return Strings::format("sampler{}",
                           expr.sampler >= 0 ? expr.sampler : expr.index);
}

std::string hlslSamplerOf(const ShaderGraph& graph, const Expr& expr)
{
    if (expr.sampler >= 0)
        return Strings::format("sampler{}", expr.sampler);

    return getHlslSamplerName(graph.textureSampling(expr.index));
}

// The combined sampler a GLSL call reads: the slot's own, or one built from
// its image and a sampler binding.
std::string glslSampledOf(const ShaderGraph& graph, const Expr& expr)
{
    if (expr.sampler < 0)
        return textureNameOf(expr);

    return Strings::format(
        "{}(textureImage{}, sampler{})",
        getGlslSampledType(graph.textureKind(expr.index),
                           graph.isComparisonSampler(expr.sampler)),
        expr.index,
        expr.sampler);
}

std::string_view metalGradientOf(TextureKind kind)
{
    if (isCubeKind(kind))
        return "gradientcube";

    return kind == TextureKind::Texture3D ? "gradient3d" : "gradient2d";
}

std::string_view hlslGatherOf(char component)
{
    switch (component)
    {
        case 'y':
            return "GatherGreen";
        case 'z':
            return "GatherBlue";
        case 'w':
            return "GatherAlpha";
        default:
            return "GatherRed";
    }
}

int glslComponentOf(char component)
{
    return (int) std::string_view {"xyzw"}.find(component);
}

// A plain read of a depth kind is a vec4 off GLSL's non-shadow sampler; the
// other two dialects hand back the one channel.
bool readsOneDepthChannel(TextureKind kind, TextureFunction function)
{
    return isDepth(kind)
           && (function == TextureFunction::Sample
               || function == TextureFunction::Level
               || function == TextureFunction::Load);
}

std::string_view textureKindTag(TextureKind kind)
{
    switch (kind)
    {
        case TextureKind::Texture2D:
            return "2D";
        case TextureKind::Cube:
            return "Cube";
        case TextureKind::Depth2D:
            return "Depth2D";
        case TextureKind::Texture2DArray:
            return "2DArray";
        case TextureKind::Depth2DArray:
            return "Depth2DArray";
        case TextureKind::Texture3D:
            return "3D";
        case TextureKind::CubeArray:
            return "CubeArray";
        case TextureKind::DepthCube:
            return "DepthCube";
        case TextureKind::DepthCubeArray:
            return "DepthCubeArray";
    }

    return "2D";
}

std::string getHlslTextureInfoName(TextureKind kind)
{
    return Strings::format("eacpTextureInfo{}", textureKindTag(kind));
}

std::string_view getHlslThirdDimension(TextureKind kind)
{
    if (kind == TextureKind::Texture3D)
        return "depth";

    return isLayered(kind) ? "layers" : "1";
}

// GetDimensions answers through out-parameters, so a size or a count read as
// an expression goes through one helper per kind: width, height, the depth or
// the layers, and the levels, at one level.
std::string getHlslTextureInfoHelper(TextureKind kind)
{
    const auto third = getHlslThirdDimension(kind);
    const auto declaresThird = third != "1";

    return Strings::format(
        "uint4 {}({} t, uint level)\n"
        "{{\n"
        "    uint width, height, {}levels;\n"
        "    t.GetDimensions(level, width, height, {}levels);\n"
        "    return uint4(width, height, {}, levels);\n"
        "}}\n\n",
        getHlslTextureInfoName(kind),
        getHlslTextureType(kind),
        declaresThird ? Strings::format("{}, ", third) : std::string {},
        declaresThird ? Strings::format("{}, ", third) : std::string {},
        third);
}

bool doesGraphQueryKind(const ShaderGraph& graph, TextureKind kind)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::TextureQuery
            && graph.textureKind(expr.index) == kind
            && !isStorageAccess(graph.textureAccess(expr.index)))
            return true;
    }

    return false;
}

bool doesGraphQuerySlot(const ShaderGraph& graph, int slot)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::TextureQuery && expr.index == slot)
            return true;
    }

    return false;
}

std::string getHlslStorageSizeName(int slot)
{
    return Strings::format("eacpStorageSize{}", slot);
}

// A storage slot's helper takes its own declared type, so it is one per slot;
// a typed UAV's GetDimensions has no level, an SRV's leads with one.
std::string getHlslStorageSizeHelper(const ShaderGraph& graph, int slot)
{
    const auto flat = graph.textureKind(slot) == TextureKind::Texture2D;
    const auto read = graph.textureAccess(slot) == TextureAccess::Read;

    return Strings::format("uint3 {}({} t)\n"
                           "{{\n"
                           "    uint width, height{}{};\n"
                           "    t.GetDimensions({}width, height{}{});\n"
                           "    return uint3(width, height, {});\n"
                           "}}\n\n",
                           getHlslStorageSizeName(slot),
                           getHlslTextureDeclarationType(graph, slot),
                           flat ? "" : ", depth",
                           read ? ", levels" : "",
                           read ? "0, " : "",
                           flat ? "" : ", depth",
                           read ? ", levels" : "",
                           flat ? "1u" : "depth");
}

// What HLSL's typed UAV hands back is as wide as the format; WGSL's load is
// four wide, the rest (0, 0, 1).
std::string widenToFour(const std::string& value,
                        int channels,
                        ValueType type,
                        Backend backend)
{
    if (channels >= 4)
        return value;

    const auto* zero = type == ValueType::Float4 ? "0.0" : "0";
    const auto* one = type == ValueType::Float4 ? "1.0" : "1";
    const auto* suffix = type == ValueType::UInt4 ? "u" : "";

    auto filled = value;

    for (auto channel = channels; channel < 3; ++channel)
        filled += Strings::format(", {}{}", zero, suffix);

    return Strings::format(
        "{}({}, {}{})", getDialectTypeName(backend, type), filled, one, suffix);
}

bool usesStorageQuery(TextureAccess access, Backend backend)
{
    if (backend == Backend::DirectX)
        return isStorageAccess(access);

    return backend == Backend::Vulkan
           && (access == TextureAccess::Write || access == TextureAccess::ReadWrite);
}

constexpr auto queriedTextureKinds =
    Array<TextureKind, 9> {TextureKind::Texture2D,
                           TextureKind::Cube,
                           TextureKind::Depth2D,
                           TextureKind::Texture2DArray,
                           TextureKind::Depth2DArray,
                           TextureKind::Texture3D,
                           TextureKind::CubeArray,
                           TextureKind::DepthCube,
                           TextureKind::DepthCubeArray};
} // namespace

std::string getTextureQueryHelpers(const ShaderGraph& graph, Backend backend)
{
    if (backend != Backend::DirectX)
        return {};

    auto definitions = std::string {};

    for (auto kind: queriedTextureKinds)
        if (doesGraphQueryKind(graph, kind))
            definitions += getHlslTextureInfoHelper(kind);

    for (auto slot = 0; slot < graph.textureCount(); ++slot)
        if (isStorageAccess(graph.textureAccess(slot))
            && doesGraphQuerySlot(graph, slot))
            definitions += getHlslStorageSizeHelper(graph, slot);

    return definitions;
}

std::string
    ExprPrinter::printStorageAddress(int slot, int coordinates, int layer) const
{
    const auto unsignedAddress = backend != Backend::Vulkan;
    const auto scalar = unsignedAddress ? ValueType::UInt : ValueType::Int;
    const auto pair = unsignedAddress ? ValueType::UInt2 : ValueType::Int2;
    const auto triple = unsignedAddress ? ValueType::UInt3 : ValueType::Int3;

    if (graph.textureKind(slot) == TextureKind::Texture3D)
        return printConverted(coordinates, triple);

    const auto texel = printConverted(coordinates, pair);

    if (layer < 0)
        return texel;

    const auto index = printConverted(layer, scalar);

    if (backend == Backend::Metal)
        return Strings::format("{}, {}", texel, index);

    return Strings::format(
        "{}({}, {})", getDialectTypeName(backend, triple), texel, index);
}

std::string ExprPrinter::printStorageLoad(const Expr& expr) const
{
    const auto layered = isLayered(graph.textureKind(expr.index));
    const auto name = textureNameOf(expr);
    const auto address =
        printStorageAddress(expr.index, expr.args[0], layered ? expr.args[1] : -1);

    if (backend == Backend::Metal)
        return Strings::format("{}.read({})", name, address);

    if (backend == Backend::Vulkan)
        return Strings::format("imageLoad({}, {})", name, address);

    return widenToFour(Strings::format("{}[{}]", name, address),
                       getStorageChannelCount(graph.textureFormat(expr.index)),
                       expr.type,
                       backend);
}

std::string ExprPrinter::printStorageQuery(const Expr& expr) const
{
    const auto name = textureNameOf(expr);
    const auto volume = graph.textureKind(expr.index) == TextureKind::Texture3D;

    if (backend == Backend::DirectX)
    {
        const auto size =
            Strings::format("{}({})", getHlslStorageSizeName(expr.index), name);

        if (expr.text == "layers")
            return size + ".z";

        return size + (volume ? ".xyz" : ".xy");
    }

    if (expr.text == "layers")
        return Strings::format("uint(imageSize({}).z)", name);

    if (volume)
        return Strings::format("uvec3(imageSize({}))", name);

    return Strings::format("uvec2(imageSize({}).xy)", name);
}

std::string ExprPrinter::printConverted(int node, ValueType type) const
{
    if (graph.expr(node).type == type)
        return printReference(node);

    return Strings::format(
        "{}({})", getDialectTypeName(backend, type), printReference(node));
}

std::string ExprPrinter::printTextureFunction(const Expr& expr) const
{
    if (backend == Backend::Metal)
        return printMetalTextureFunction(expr);

    if (backend == Backend::DirectX)
        return printHlslTextureFunction(expr);

    return printGlslTextureFunction(expr);
}

// The coordinate HLSL and GLSL take: an array's layer rides in it as a float.
std::string ExprPrinter::printTextureLocation(const Expr& expr) const
{
    const auto kind = graph.textureKind(expr.index);
    const auto coordinate = printReference(expr.args[0]);

    if (!isLayered(kind))
        return coordinate;

    const auto type = isCubeKind(kind) ? ValueType::Float4 : ValueType::Float3;

    return Strings::format("{}({}, {})",
                           getDialectTypeName(backend, type),
                           coordinate,
                           printConverted(expr.args[1], ValueType::Float));
}

std::string ExprPrinter::printMetalTextureFunction(const Expr& expr) const
{
    const auto kind = graph.textureKind(expr.index);
    const auto function = textureFunctionNamed(expr.text);
    const auto name = textureNameOf(expr);
    const auto sampler = metalSamplerOf(expr);

    if (function == TextureFunction::Load)
        return printTexelLoad(expr);

    auto location = printReference(expr.args[0]);

    if (isLayered(kind))
        location += ", " + printConverted(expr.args[1], ValueType::UInt);

    auto extra = [&](int which)
    { return printReference(textureExtra(graph, expr, which)); };
    auto extraFloat = [&](int which)
    { return printConverted(textureExtra(graph, expr, which), ValueType::Float); };

    switch (function)
    {
        case TextureFunction::Level:
            return Strings::format("{}.sample({}, {}, level({}))",
                                   name,
                                   sampler,
                                   location,
                                   extraFloat(0));

        case TextureFunction::Bias:
            return Strings::format("{}.sample({}, {}, bias({}))",
                                   name,
                                   sampler,
                                   location,
                                   extraFloat(0));

        case TextureFunction::Grad:
            return Strings::format("{}.sample({}, {}, {}({}, {}))",
                                   name,
                                   sampler,
                                   location,
                                   metalGradientOf(kind),
                                   extra(0),
                                   extra(1));

        case TextureFunction::Compare:
            return Strings::format("{}.sample_compare({}, {}, {})",
                                   name,
                                   sampler,
                                   location,
                                   extraFloat(0));

        case TextureFunction::CompareLevel:
            return Strings::format("{}.sample_compare({}, {}, {}, level(0))",
                                   name,
                                   sampler,
                                   location,
                                   extraFloat(0));

        case TextureFunction::Gather:
            if (isDepth(kind))
                return Strings::format("{}.gather({}, {})", name, sampler, location);

            return Strings::format("{}.gather({}, {}, {}component::{})",
                                   name,
                                   sampler,
                                   location,
                                   isCubeKind(kind) ? "" : "int2(0), ",
                                   expr.op);

        case TextureFunction::GatherCompare:
            return Strings::format("{}.gather_compare({}, {}, {})",
                                   name,
                                   sampler,
                                   location,
                                   extraFloat(0));

        default:
            return Strings::format("{}.sample({}, {})", name, sampler, location);
    }
}

// HLSL's comparisons are level-zero ones whichever WGSL function asked, the
// derived level of a one-level depth view being 0 anyway.
std::string ExprPrinter::printHlslTextureFunction(const Expr& expr) const
{
    const auto kind = graph.textureKind(expr.index);
    const auto function = textureFunctionNamed(expr.text);
    const auto name = textureNameOf(expr);
    const auto sampler = hlslSamplerOf(graph, expr);

    if (function == TextureFunction::Load)
        return printTexelLoad(expr);

    const auto location = printTextureLocation(expr);

    auto extra = [&](int which)
    { return printReference(textureExtra(graph, expr, which)); };
    auto extraFloat = [&](int which)
    { return printConverted(textureExtra(graph, expr, which), ValueType::Float); };

    switch (function)
    {
        case TextureFunction::Level:
            return Strings::format("{}.SampleLevel({}, {}, {})",
                                   name,
                                   sampler,
                                   location,
                                   extraFloat(0));

        case TextureFunction::Bias:
            return Strings::format(
                "{}.SampleBias({}, {}, {})", name, sampler, location, extraFloat(0));

        case TextureFunction::Grad:
            return Strings::format("{}.SampleGrad({}, {}, {}, {})",
                                   name,
                                   sampler,
                                   location,
                                   extra(0),
                                   extra(1));

        case TextureFunction::Compare:
        case TextureFunction::CompareLevel:
            return Strings::format("{}.SampleCmpLevelZero({}, {}, {})",
                                   name,
                                   sampler,
                                   location,
                                   extraFloat(0));

        case TextureFunction::Gather:
            return Strings::format("{}.{}({}, {})",
                                   name,
                                   isDepth(kind) ? "Gather" : hlslGatherOf(expr.op),
                                   sampler,
                                   location);

        case TextureFunction::GatherCompare:
            return Strings::format(
                "{}.GatherCmp({}, {}, {})", name, sampler, location, extraFloat(0));

        default:
            return Strings::format("{}.Sample({}, {})", name, sampler, location);
    }
}

// The shadow samplers take the reference inside the coordinate, except a cube
// array's, whose coordinate is already four wide.
std::string ExprPrinter::printGlslTextureCompare(const Expr& expr) const
{
    const auto kind = graph.textureKind(expr.index);
    const auto name = glslSampledOf(graph, expr);
    const auto coordinate = printReference(expr.args[0]);
    const auto reference =
        printConverted(textureExtra(graph, expr, 0), ValueType::Float);

    if (kind == TextureKind::DepthCubeArray)
        return Strings::format(
            "texture({}, {}, {})", name, printTextureLocation(expr), reference);

    if (kind == TextureKind::Depth2DArray)
        return Strings::format("texture({}, vec4({}, {}, {}))",
                               name,
                               coordinate,
                               printConverted(expr.args[1], ValueType::Float),
                               reference);

    const auto wide = kind == TextureKind::DepthCube ? "vec4" : "vec3";
    return Strings::format(
        "texture({}, {}({}, {}))", name, wide, coordinate, reference);
}

std::string ExprPrinter::printGlslTextureFunction(const Expr& expr) const
{
    const auto kind = graph.textureKind(expr.index);
    const auto function = textureFunctionNamed(expr.text);
    const auto name = glslSampledOf(graph, expr);

    if (function == TextureFunction::Load)
        return printTexelLoad(expr);

    if (function == TextureFunction::Compare
        || function == TextureFunction::CompareLevel)
        return printGlslTextureCompare(expr);

    const auto location = printTextureLocation(expr);

    auto extra = [&](int which)
    { return printReference(textureExtra(graph, expr, which)); };
    auto extraFloat = [&](int which)
    { return printConverted(textureExtra(graph, expr, which), ValueType::Float); };

    auto call = std::string {};

    switch (function)
    {
        case TextureFunction::Level:
            call = Strings::format(
                "textureLod({}, {}, {})", name, location, extraFloat(0));
            break;

        case TextureFunction::Bias:
            call = Strings::format(
                "texture({}, {}, {})", name, location, extraFloat(0));
            break;

        case TextureFunction::Grad:
            call = Strings::format(
                "textureGrad({}, {}, {}, {})", name, location, extra(0), extra(1));
            break;

        case TextureFunction::Gather:
            call = isDepth(kind)
                       ? Strings::format("textureGather({}, {})", name, location)
                       : Strings::format("textureGather({}, {}, {})",
                                         name,
                                         location,
                                         glslComponentOf(expr.op));
            break;

        case TextureFunction::GatherCompare:
            call = Strings::format(
                "textureGather({}, {}, {})", name, location, extraFloat(0));
            break;

        default:
            call = Strings::format("texture({}, {})", name, location);
            break;
    }

    return readsOneDepthChannel(kind, function) ? call + ".r" : call;
}

// Metal reads at unsigned coordinates, the other two at signed ones with the
// level (and the layer) packed beside them.
std::string ExprPrinter::printTexelLoad(const Expr& expr) const
{
    if (graph.textureAccess(expr.index) == TextureAccess::ReadWrite)
        return printStorageLoad(expr);

    const auto kind = graph.textureKind(expr.index);
    const auto name = textureNameOf(expr);
    const auto volume = kind == TextureKind::Texture3D;
    const auto level = textureExtra(graph, expr, 0);
    const auto layered = isLayered(kind);

    if (backend == Backend::Metal)
    {
        const auto coordinates = printConverted(
            expr.args[0], volume ? ValueType::UInt3 : ValueType::UInt2);
        const auto layer =
            layered ? printConverted(expr.args[1], ValueType::UInt) + ", " : "";

        return Strings::format("{}.read({}, {}{})",
                               name,
                               coordinates,
                               layer,
                               printConverted(level, ValueType::UInt));
    }

    const auto coordinates =
        printConverted(expr.args[0], volume ? ValueType::Int3 : ValueType::Int2);
    const auto signedLevel = printConverted(level, ValueType::Int);
    const auto layer =
        layered ? printConverted(expr.args[1], ValueType::Int) + ", " : "";

    if (backend == Backend::DirectX)
    {
        const auto packed = volume || layered ? ValueType::Int4 : ValueType::Int3;

        return Strings::format("{}.Load({}({}, {}{}))",
                               name,
                               getDialectTypeName(backend, packed),
                               coordinates,
                               layer,
                               signedLevel);
    }

    const auto address =
        layered ? Strings::format("ivec3({}, {})",
                                  coordinates,
                                  printConverted(expr.args[1], ValueType::Int))
                : coordinates;
    const auto call =
        Strings::format("texelFetch({}, {}, {})", name, address, signedLevel);

    return isDepth(kind) ? call + ".r" : call;
}

std::string ExprPrinter::printTextureQuery(const Expr& expr) const
{
    if (usesStorageQuery(graph.textureAccess(expr.index), backend))
        return printStorageQuery(expr);

    const auto kind = graph.textureKind(expr.index);
    const auto name = textureNameOf(expr);
    const auto volume = kind == TextureKind::Texture3D;
    const auto hasLevel = expr.args.size() > 0;

    if (backend == Backend::Metal)
    {
        if (expr.text == "levels")
            return name + ".get_num_mip_levels()";

        if (expr.text == "layers")
            return name + ".get_array_size()";

        const auto level = hasLevel ? printConverted(expr.args[0], ValueType::UInt)
                                    : std::string {};

        if (volume)
            return Strings::format(
                "uint3({0}.get_width({1}), {0}.get_height({1}), {0}.get_depth({1}))",
                name,
                level);

        return Strings::format(
            "uint2({0}.get_width({1}), {0}.get_height({1}))", name, level);
    }

    if (backend == Backend::DirectX)
    {
        const auto level =
            hasLevel ? printConverted(expr.args[0], ValueType::UInt) : "0u";
        const auto info =
            Strings::format("{}({}, {})", getHlslTextureInfoName(kind), name, level);

        if (expr.text == "levels")
            return info + ".w";

        if (expr.text == "layers")
            return info + ".z";

        return info + (volume ? ".xyz" : ".xy");
    }

    if (expr.text == "levels")
        return Strings::format("uint(textureQueryLevels({}))", name);

    if (expr.text == "layers")
        return Strings::format("uint(textureSize({}, 0).z)", name);

    const auto level =
        hasLevel ? printConverted(expr.args[0], ValueType::Int) : std::string {"0"};

    if (volume)
        return Strings::format("uvec3(textureSize({}, {}))", name, level);

    return Strings::format("uvec2(textureSize({}, {}).xy)", name, level);
}
} // namespace eacp::GPU::ShaderEmission
