#include "VulkanTypes.h"

#include <cstdint>
#include <optional>

namespace eacp::GPU
{
namespace
{
// SPIR-V's OpTypeImage Dim operand.
enum class SpirvDim : std::uint32_t
{
    oneD = 0,
    twoD = 1,
    threeD = 2,
    cube = 3
};

struct SpirvViewType
{
    SpirvDim dim;
    bool arrayed;
    VkImageViewType viewType;
};

constexpr SpirvViewType spirvViewTypes[] = {
    {SpirvDim::oneD, false, VK_IMAGE_VIEW_TYPE_1D},
    {SpirvDim::oneD, true, VK_IMAGE_VIEW_TYPE_1D_ARRAY},
    {SpirvDim::twoD, false, VK_IMAGE_VIEW_TYPE_2D},
    {SpirvDim::twoD, true, VK_IMAGE_VIEW_TYPE_2D_ARRAY},
    {SpirvDim::threeD, false, VK_IMAGE_VIEW_TYPE_3D},
    {SpirvDim::cube, false, VK_IMAGE_VIEW_TYPE_CUBE},
    {SpirvDim::cube, true, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY},
};

// The view an OpTypeImage's Dim and Arrayed operands read through. Rect,
// Buffer and SubpassData have none here: eacp's emitters never declare them,
// and a slot declaring one keeps the 2D placeholder.
VkImageViewType getViewTypeForSpirvImage(std::uint32_t dim, bool arrayed)
{
    for (const auto& entry: spirvViewTypes)
    {
        if (static_cast<std::uint32_t>(entry.dim) == dim && entry.arrayed == arrayed)
            return entry.viewType;
    }

    return VK_IMAGE_VIEW_TYPE_2D;
}

// What an OpTypeImage's Sampled Type operand names.
enum class SpirvComponents
{
    floatingPoint,
    unsignedInteger,
    signedInteger
};

struct SpirvComponentFormat
{
    SpirvComponents components;
    VkFormat format;
};

// Every one of these is sampleable and storable on any Vulkan device.
constexpr SpirvComponentFormat spirvComponentFormats[] = {
    {SpirvComponents::floatingPoint, VK_FORMAT_R8G8B8A8_UNORM},
    {SpirvComponents::unsignedInteger, VK_FORMAT_R8G8B8A8_UINT},
    {SpirvComponents::signedInteger, VK_FORMAT_R8G8B8A8_SINT},
};

VkFormat getFormatForSpirvComponents(SpirvComponents components)
{
    for (const auto& entry: spirvComponentFormats)
    {
        if (entry.components == components)
            return entry.format;
    }

    return VK_FORMAT_R8G8B8A8_UNORM;
}

struct SpirvImageFormat
{
    std::uint32_t imageFormat;
    VkFormat format;
};

// SPIR-V's Image Format operand: every format the specification lists but the
// two 64-bit ones, 1 to 39.
constexpr SpirvImageFormat spirvImageFormats[] = {
    {1, VK_FORMAT_R32G32B32A32_SFLOAT},
    {2, VK_FORMAT_R16G16B16A16_SFLOAT},
    {3, VK_FORMAT_R32_SFLOAT},
    {4, VK_FORMAT_R8G8B8A8_UNORM},
    {5, VK_FORMAT_R8G8B8A8_SNORM},
    {6, VK_FORMAT_R32G32_SFLOAT},
    {7, VK_FORMAT_R16G16_SFLOAT},
    {8, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
    {9, VK_FORMAT_R16_SFLOAT},
    {10, VK_FORMAT_R16G16B16A16_UNORM},
    {11, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
    {12, VK_FORMAT_R16G16_UNORM},
    {13, VK_FORMAT_R8G8_UNORM},
    {14, VK_FORMAT_R16_UNORM},
    {15, VK_FORMAT_R8_UNORM},
    {16, VK_FORMAT_R16G16B16A16_SNORM},
    {17, VK_FORMAT_R16G16_SNORM},
    {18, VK_FORMAT_R8G8_SNORM},
    {19, VK_FORMAT_R16_SNORM},
    {20, VK_FORMAT_R8_SNORM},
    {21, VK_FORMAT_R32G32B32A32_SINT},
    {22, VK_FORMAT_R16G16B16A16_SINT},
    {23, VK_FORMAT_R8G8B8A8_SINT},
    {24, VK_FORMAT_R32_SINT},
    {25, VK_FORMAT_R32G32_SINT},
    {26, VK_FORMAT_R16G16_SINT},
    {27, VK_FORMAT_R8G8_SINT},
    {28, VK_FORMAT_R16_SINT},
    {29, VK_FORMAT_R8_SINT},
    {30, VK_FORMAT_R32G32B32A32_UINT},
    {31, VK_FORMAT_R16G16B16A16_UINT},
    {32, VK_FORMAT_R8G8B8A8_UINT},
    {33, VK_FORMAT_R32_UINT},
    {34, VK_FORMAT_A2B10G10R10_UINT_PACK32},
    {35, VK_FORMAT_R32G32_UINT},
    {36, VK_FORMAT_R16G16_UINT},
    {37, VK_FORMAT_R8G8_UINT},
    {38, VK_FORMAT_R16_UINT},
    {39, VK_FORMAT_R8_UINT},
};

// The format a placeholder for this image takes: the declared storage format
// where there is one, a depth format for a shadow sampler, and otherwise one
// of the declared component type.
VkFormat getPlaceholderFormat(std::uint32_t imageFormat,
                              bool isDepthCompare,
                              SpirvComponents components)
{
    if (isDepthCompare)
        return VK_FORMAT_D16_UNORM;

    for (const auto& entry: spirvImageFormats)
    {
        if (entry.imageFormat == imageFormat)
            return entry.format;
    }

    return getFormatForSpirvComponents(components);
}

// A multisampled image's placeholder has more than one sample, which is all a
// sampler2DMS asks of it. Four is guaranteed for float colour formats only; on
// a device without it for an integer format the placeholder is not made, is
// logged, and the slot is left unwritten.
constexpr auto placeholderSampleCount = VK_SAMPLE_COUNT_4_BIT;

// An OpTypeInt at `index`: its Signedness operand follows the width.
SpirvComponents readIntegerComponents(const Vector<std::uint32_t>& words, int index)
{
    if (words[index + 3] != 0u)
        return SpirvComponents::signedInteger;

    return SpirvComponents::unsignedInteger;
}

// An OpTypeImage at `index`: Sampled Type, Dim, Depth, Arrayed, MS, Sampled and
// Image Format follow its result id.
VulkanImageShape readImageShape(const Vector<std::uint32_t>& words,
                                int index,
                                const Vector<SpirvComponents>& components)
{
    const auto sampledType = words[index + 2];
    const auto dim = words[index + 3];
    const auto isDepthCompare = words[index + 4] == 1u;
    const auto isArrayed = words[index + 5] == 1u;
    const auto isMultisampled = words[index + 6] == 1u;
    const auto imageFormat = words[index + 8];

    const auto typeCount = static_cast<std::uint32_t>(components.size());
    const auto isSampledTypeKnown = sampledType < typeCount;
    auto sampledComponents = SpirvComponents::floatingPoint;

    if (isSampledTypeKnown)
        sampledComponents = components[static_cast<int>(sampledType)];

    auto shape = VulkanImageShape {};
    shape.viewType = getViewTypeForSpirvImage(dim, isArrayed);
    shape.format =
        getPlaceholderFormat(imageFormat, isDepthCompare, sampledComponents);

    if (isMultisampled)
        shape.samples = placeholderSampleCount;

    return shape;
}

constexpr auto opTypeInt = std::uint32_t {21};
constexpr auto opTypeImage = std::uint32_t {25};
constexpr auto opTypeSampler = std::uint32_t {26};
constexpr auto opTypeSampledImage = std::uint32_t {27};
constexpr auto opTypePointer = std::uint32_t {32};
constexpr auto opVariable = std::uint32_t {59};
constexpr auto opDecorate = std::uint32_t {71};
constexpr auto decorationBinding = std::uint32_t {33};

// OpTypeImage's Sampled operand: 1 is read through a sampler, 2 read or
// written with the image instructions.
constexpr auto sampledThroughASampler = std::uint32_t {1};

// Magic number, version, generator, id bound, schema.
constexpr auto headerWords = 5;

enum class IdKind
{
    unknown,
    sampledImage, // OpTypeSampledImage: a combined image sampler
    storageImage, // OpTypeImage with Sampled = 2
    readImage, // OpTypeImage with Sampled = 1, unpaired
    sampler // OpTypeSampler: a sampler binding of its own
};

struct DescriptorOfKind
{
    IdKind kind;
    VkDescriptorType type;
};

constexpr DescriptorOfKind descriptorsOfKinds[] = {
    {IdKind::sampledImage, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
    {IdKind::storageImage, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    {IdKind::readImage, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {IdKind::sampler, VK_DESCRIPTOR_TYPE_SAMPLER},
};

// What one module's instructions say about its texture bindings, collected
// per id rather than resolved inline: a module puts its annotations ahead of
// its types.
class SpirvTextureDeclarations
{
public:
    SpirvTextureDeclarations(const Vector<std::uint32_t>& moduleWords,
                             int firstBindingOfTextures)
        : words(moduleWords)
        , firstBinding(firstBindingOfTextures)
        , bound(static_cast<int>(moduleWords[3]))
    {
        kinds.resize(bound, IdKind::unknown);
        components.resize(bound, SpirvComponents::floatingPoint);
        imageShapes.resize(bound, VulkanImageShape {});
        wrappedImage.resize(bound, 0u);
        pointee.resize(bound, 0u);
        variableType.resize(bound, 0u);
        slotOfId.resize(bound, -1);
    }

    void readInstructions()
    {
        auto index = headerWords;

        while (index < words.size())
        {
            const auto instruction = words[index];
            const auto wordCount = static_cast<int>(instruction >> 16);
            const auto opcode = instruction & 0xffffu;

            // A zero-length instruction cannot be stepped over, and one past the
            // end means the module is not what it says it is.
            if (wordCount <= 0 || index + wordCount > words.size())
                break;

            readInstruction(opcode, index, wordCount);
            index += wordCount;
        }
    }

    VulkanTextureBindings makeBindings() const
    {
        auto bindings = VulkanTextureBindings {};

        for (auto id = 0; id < bound; ++id)
            addBinding(bindings, id);

        return bindings;
    }

private:
    bool isInRange(std::uint32_t id) const
    {
        const auto idBound = static_cast<std::uint32_t>(bound);
        return id < idBound;
    }

    // The type declarations read here and the fewest words each has, its
    // result id among them.
    struct TypeDeclaration
    {
        std::uint32_t opcode;
        int minimumWords;
    };

    static constexpr TypeDeclaration typeDeclarations[] = {
        {opTypeInt, 4},
        {opTypeImage, 9},
        {opTypeSampler, 2},
        {opTypeSampledImage, 3},
        {opTypePointer, 4},
    };

    bool isReadableTypeDeclaration(std::uint32_t opcode,
                                   int index,
                                   int wordCount) const
    {
        for (const auto& declaration: typeDeclarations)
        {
            if (declaration.opcode != opcode)
                continue;

            const auto hasEveryOperand = wordCount >= declaration.minimumWords;
            return hasEveryOperand && isInRange(words[index + 1]);
        }

        return false;
    }

    bool isBindingDecoration(std::uint32_t opcode, int index, int wordCount) const
    {
        return opcode == opDecorate && wordCount >= 4
               && words[index + 2] == decorationBinding
               && isInRange(words[index + 1]);
    }

    bool isReadableVariable(std::uint32_t opcode, int index, int wordCount) const
    {
        return opcode == opVariable && wordCount >= 4 && isInRange(words[index + 2]);
    }

    void readInstruction(std::uint32_t opcode, int index, int wordCount)
    {
        if (isBindingDecoration(opcode, index, wordCount))
            readBinding(index);
        else if (isReadableTypeDeclaration(opcode, index, wordCount))
            readTypeDeclaration(opcode, index);
        else if (isReadableVariable(opcode, index, wordCount))
            variableType[static_cast<int>(words[index + 2])] = words[index + 1];
    }

    void readTypeDeclaration(std::uint32_t opcode, int index)
    {
        const auto id = static_cast<int>(words[index + 1]);

        switch (opcode)
        {
            case opTypeImage:
                readImageType(index);
                break;

            case opTypeInt:
                components[id] = readIntegerComponents(words, index);
                break;

            case opTypeSampledImage:
                kinds[id] = IdKind::sampledImage;
                wrappedImage[id] = words[index + 2];
                break;

            case opTypeSampler:
                kinds[id] = IdKind::sampler;
                break;

            case opTypePointer:
                pointee[id] = words[index + 3];
                break;

            default:
                break;
        }
    }

    void readBinding(int index)
    {
        const auto slot = static_cast<int>(words[index + 3]) - firstBinding;

        if (slot >= 0 && slot < maxTextureSlots)
            slotOfId[static_cast<int>(words[index + 1])] = slot;
    }

    void readImageType(int index)
    {
        const auto id = static_cast<int>(words[index + 1]);

        kinds[id] = IdKind::storageImage;

        if (words[index + 7] == sampledThroughASampler)
            kinds[id] = IdKind::readImage;

        imageShapes[id] = readImageShape(words, index, components);
    }

    // The type a texture variable points at, or nothing for any other id.
    std::optional<std::uint32_t> getTextureType(int id) const
    {
        if (slotOfId[id] < 0 || variableType[id] == 0u
            || !isInRange(variableType[id]))
        {
            return std::nullopt;
        }

        const auto pointed = pointee[static_cast<int>(variableType[id])];

        if (!isInRange(pointed))
            return std::nullopt;

        return pointed;
    }

    VulkanImageShape getShapeOf(std::uint32_t type) const
    {
        auto image = type;

        if (kinds[static_cast<int>(type)] == IdKind::sampledImage)
            image = wrappedImage[static_cast<int>(type)];

        if (!isInRange(image))
            return {};

        return imageShapes[static_cast<int>(image)];
    }

    void addBinding(VulkanTextureBindings& bindings, int id) const
    {
        const auto type = getTextureType(id);

        if (!type)
            return;

        const auto kind = kinds[static_cast<int>(*type)];

        for (const auto& entry: descriptorsOfKinds)
        {
            if (entry.kind != kind)
                continue;

            if (kind == IdKind::sampler)
                bindings.add(slotOfId[id], entry.type);
            else
                bindings.add(slotOfId[id], entry.type, getShapeOf(*type));
        }
    }

    const Vector<std::uint32_t>& words;
    const int firstBinding;
    const int bound;

    Vector<IdKind> kinds;

    // For an OpTypeInt, the components an image of it holds; floating point
    // for every other id, an OpTypeFloat included.
    Vector<SpirvComponents> components;

    // For an OpTypeImage, the view a placeholder for it needs.
    Vector<VulkanImageShape> imageShapes;

    // For an OpTypeSampledImage, the image type it wraps.
    Vector<std::uint32_t> wrappedImage;

    // For an OpTypePointer, the id of what it points at; 0 for everything else.
    Vector<std::uint32_t> pointee;

    Vector<std::uint32_t> variableType;
    Vector<int> slotOfId;
};
} // namespace

VulkanTextureBindings spirvTextureBindings(const Vector<std::uint32_t>& words,
                                           int firstBinding)
{
    if (words.size() <= headerWords || static_cast<int>(words[3]) <= 0)
        return {};

    auto declarations = SpirvTextureDeclarations {words, firstBinding};
    declarations.readInstructions();
    return declarations.makeBindings();
}
} // namespace eacp::GPU
