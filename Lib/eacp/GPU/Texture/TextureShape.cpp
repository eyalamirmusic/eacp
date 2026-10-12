#include "TextureShape.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>

namespace eacp::GPU
{
namespace
{
std::optional<TextureType> resolvedKind(const TextureDescriptor& descriptor)
{
    if (!descriptor.cube)
        return descriptor.kind;

    if (descriptor.kind == TextureType::TwoD || descriptor.kind == TextureType::Cube)
        return TextureType::Cube;

    return {};
}

bool takesLayers(TextureType kind)
{
    return kind == TextureType::TwoDArray || kind == TextureType::CubeArray;
}

bool isCube(TextureType kind)
{
    return kind == TextureType::Cube || kind == TextureType::CubeArray;
}

bool shapeIsPossible(const TextureDescriptor& descriptor, TextureType kind)
{
    if (descriptor.width <= 0 || descriptor.height <= 0)
        return false;

    if (descriptor.arrayLength < 1 || descriptor.volumeDepth < 1)
        return false;

    if (descriptor.arrayLength > 1 && !takesLayers(kind))
        return false;

    if (descriptor.volumeDepth > 1 && kind != TextureType::ThreeD)
        return false;

    if (isCube(kind) && descriptor.width != descriptor.height)
        return false;

    if (descriptor.renderTarget && descriptor.sampleCount > 1
        && (kind != TextureType::TwoD || descriptor.mipLevels > 1))
        return false;

    if (kind == TextureType::ThreeD && isCompressedFormat(descriptor.format))
        return false;

    // A volume is rendered into one z at a time and has no depth plane to go
    // with it: no API makes a 3D depth texture.
    if (kind == TextureType::ThreeD && descriptor.renderTarget
        && (descriptor.depth || descriptor.stencil || descriptor.sampleableDepth))
        return false;

    return true;
}

bool isGpuFilled(const TextureDescriptor& descriptor)
{
    return descriptor.renderTarget || descriptor.computeWrite;
}

bool isOnBlockGrid(int origin, int extent, int levelExtent)
{
    return origin % 4 == 0 && (extent % 4 == 0 || origin + extent == levelExtent);
}

const unsigned char* bytesAt(const void* pixels, std::size_t offset)
{
    return static_cast<const unsigned char*>(pixels) + offset;
}

SubresourceUpload wholeLevelOf(const TextureShape& shape,
                               TextureFormat format,
                               int level,
                               int slice,
                               const void* pixels,
                               int bytesPerRow,
                               int bytesPerImage)
{
    auto upload = SubresourceUpload {};
    upload.level = level;
    upload.slice = slice;
    upload.width = shape.levelWidth(level);
    upload.height = shape.levelHeight(level);
    upload.depth = shape.levelDepth(level);
    upload.pixels = pixels;
    upload.bytesPerRow =
        bytesPerRow > 0 ? bytesPerRow : levelBytesPerRow(format, upload.width);
    upload.bytesPerImage =
        bytesPerImage > 0 ? bytesPerImage
                          : upload.bytesPerRow * levelRows(format, upload.height);
    return upload;
}

void addBuiltChain(TextureUploadPlan& plan,
                   const TextureShape& shape,
                   TextureFormat format,
                   int slice,
                   const void* pixels,
                   int bytesPerRow)
{
    auto chain = std::make_unique<MipChain>(
        buildMipChain(pixels, shape.width, shape.height, format, bytesPerRow));

    if (!chain->isValid())
    {
        plan.valid = false;
        return;
    }

    const auto levels = std::min(chain->levelCount(), shape.levels);

    for (auto level = 0; level < levels; ++level)
        plan.uploads.add(
            wholeLevelOf(shape, format, level, slice, chain->level(level), 0, 0));

    plan.chains.add(std::move(chain));
}

void addPackedSlice(TextureUploadPlan& plan,
                    const TextureShape& shape,
                    TextureFormat format,
                    int slice,
                    const void* pixels,
                    int bytesPerRow)
{
    if (shape.buildsChain())
    {
        addBuiltChain(plan, shape, format, slice, pixels, bytesPerRow);
        return;
    }

    auto offset = std::size_t {0};

    for (auto level = 0; level < shape.levels; ++level)
    {
        const auto upload = wholeLevelOf(
            shape, format, level, slice, bytesAt(pixels, offset), bytesPerRow, 0);

        plan.uploads.add(upload);
        offset += (std::size_t) upload.bytesPerImage * (std::size_t) upload.depth;
    }
}

int extentToEdge(int requested, int origin, int levelExtent)
{
    return requested > 0 ? requested : levelExtent - origin;
}

std::optional<SubresourceUpload> planRegion(const TextureShape& shape,
                                            TextureFormat format,
                                            const TextureRegion& region)
{
    if (region.level < 0 || region.level >= shape.levels || region.slice < 0
        || region.slice >= shape.slices())
        return {};

    if (region.bytesPerRow < 0 || region.bytesPerImage < 0)
        return {};

    const auto levelWidth = shape.levelWidth(region.level);
    const auto levelHeight = shape.levelHeight(region.level);
    const auto levelDepth = shape.levelDepth(region.level);

    auto upload = SubresourceUpload {};
    upload.level = region.level;
    upload.slice = region.slice;
    upload.x = region.origin.x;
    upload.y = region.origin.y;
    upload.z = region.origin.z;
    upload.width = extentToEdge(region.size.width, upload.x, levelWidth);
    upload.height = extentToEdge(region.size.height, upload.y, levelHeight);
    upload.depth = extentToEdge(region.size.depth, upload.z, levelDepth);

    if (!isWithin(upload.x, upload.width, levelWidth)
        || !isWithin(upload.y, upload.height, levelHeight)
        || !isWithin(upload.z, upload.depth, levelDepth))
        return {};

    if (isCompressedFormat(format)
        && (!isOnBlockGrid(upload.x, upload.width, levelWidth)
            || !isOnBlockGrid(upload.y, upload.height, levelHeight)))
        return {};

    const auto tightRow = levelBytesPerRow(format, upload.width);
    upload.bytesPerRow = region.bytesPerRow > 0 ? region.bytesPerRow : tightRow;

    const auto tightImage = upload.bytesPerRow * levelRows(format, upload.height);
    upload.bytesPerImage =
        region.bytesPerImage > 0 ? region.bytesPerImage : tightImage;

    if (upload.bytesPerRow < tightRow || upload.bytesPerImage < tightImage)
        return {};

    return upload;
}
} // namespace

bool TextureShape::isCubeKind() const
{
    return kind == TextureType::Cube || kind == TextureType::CubeArray;
}

bool TextureShape::isArrayKind() const
{
    return kind == TextureType::TwoDArray || kind == TextureType::CubeArray;
}

bool TextureShape::isVolume() const
{
    return kind == TextureType::ThreeD;
}

int TextureShape::slices() const
{
    return layers * (isCubeKind() ? 6 : 1);
}

int TextureShape::levelWidth(int level) const
{
    return mipExtent(width, level);
}

int TextureShape::levelHeight(int level) const
{
    return mipExtent(height, level);
}

int TextureShape::levelDepth(int level) const
{
    return isVolume() ? mipExtent(depth, level) : 1;
}

bool TextureShape::buildsChain() const
{
    return levels > 1 && !explicitLevels;
}

int maxMipLevels(TextureType kind, int width, int height, int depth)
{
    const auto largest =
        kind == TextureType::ThreeD ? std::max(width, depth) : width;
    return mipLevelCount(largest, height);
}

std::optional<TextureShape> resolveTextureShape(const TextureDescriptor& descriptor,
                                                InitialData data)
{
    const auto kind = resolvedKind(descriptor);

    if (!kind || !shapeIsPossible(descriptor, *kind))
        return {};

    auto shape = TextureShape {};
    shape.kind = *kind;
    shape.width = descriptor.width;
    shape.height = descriptor.height;
    shape.depth = *kind == TextureType::ThreeD ? descriptor.volumeDepth : 1;
    shape.layers = takesLayers(*kind) ? descriptor.arrayLength : 1;

    if (descriptor.mipLevels < 0)
        return {};

    if (descriptor.mipLevels > 0)
    {
        if (descriptor.mipmapped)
            return {};

        if (descriptor.mipLevels
            > maxMipLevels(shape.kind, shape.width, shape.height, shape.depth))
            return {};

        if (data == InitialData::None && !isGpuFilled(descriptor))
            return {};

        shape.levels = descriptor.mipLevels;
        shape.explicitLevels = true;
        return shape;
    }

    if (descriptor.mipmapped && data != InitialData::None
        && canBuildMipChain(descriptor.format) && !shape.isVolume())
        shape.levels = mipLevelCount(shape.width, shape.height);

    return shape;
}

TextureUploadPlan planWholeUpload(const TextureShape& shape,
                                  TextureFormat format,
                                  const void* pixels,
                                  int bytesPerRow)
{
    auto plan = TextureUploadPlan {};

    if (pixels == nullptr || bytesPerRow < 0
        || (bytesPerRow != 0
            && (shape.explicitLevels || isCompressedFormat(format))))
    {
        plan.valid = false;
        return plan;
    }

    if (shape.isVolume())
    {
        addPackedSlice(plan, shape, format, 0, pixels, bytesPerRow);
        return plan;
    }

    const auto stride =
        bytesPerRow > 0 ? bytesPerRow : levelBytesPerRow(format, shape.width);
    auto sliceBytes =
        (std::size_t) stride * (std::size_t) levelRows(format, shape.height);

    if (shape.explicitLevels)
    {
        sliceBytes = 0;

        for (auto level = 0; level < shape.levels; ++level)
            sliceBytes += (std::size_t) levelBytes(
                format, shape.levelWidth(level), shape.levelHeight(level));
    }

    for (auto slice = 0; slice < shape.slices() && plan.valid; ++slice)
        addPackedSlice(plan,
                       shape,
                       format,
                       slice,
                       bytesAt(pixels, (std::size_t) slice * sliceBytes),
                       bytesPerRow);

    return plan;
}

TextureUploadPlan
    planSubresourceUploads(const TextureShape& shape,
                           TextureFormat format,
                           std::span<const TextureSubresource> subresources)
{
    auto plan = TextureUploadPlan {};

    for (const auto& subresource: subresources)
    {
        auto region =
            TextureRegion::wholeLevel(subresource.level, subresource.slice);
        region.bytesPerRow = subresource.bytesPerRow;
        region.bytesPerImage = subresource.bytesPerImage;

        const auto upload = planRegion(shape, format, region);

        if (!upload || subresource.pixels == nullptr)
        {
            LOG("eacp: a texture's initial subresource names a level or slice it "
                "lacks, or carries no pixels");
            plan.valid = false;
            return plan;
        }

        if (shape.buildsChain() && subresource.level == 0)
        {
            addBuiltChain(plan,
                          shape,
                          format,
                          subresource.slice,
                          subresource.pixels,
                          upload->bytesPerRow);
            continue;
        }

        auto withPixels = *upload;
        withPixels.pixels = subresource.pixels;
        plan.uploads.add(withPixels);
    }

    return plan;
}

std::optional<SubresourceUpload> planRegionUpload(const TextureShape& shape,
                                                  TextureFormat format,
                                                  const TextureRegion& region,
                                                  const void* pixels)
{
    if (pixels == nullptr)
        return {};

    auto upload = planRegion(shape, format, region);

    if (upload)
        upload->pixels = pixels;

    return upload;
}

std::optional<SubresourceUpload> planRegionRead(const TextureShape& shape,
                                                TextureFormat format,
                                                const TextureRegion& region,
                                                void* dst)
{
    if (dst == nullptr || isCompressedFormat(format))
        return {};

    auto read = planRegion(shape, format, region);

    if (read)
        read->pixels = dst;

    return read;
}
} // namespace eacp::GPU
