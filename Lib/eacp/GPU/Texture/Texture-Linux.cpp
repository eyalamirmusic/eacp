#include "Texture.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"
#include "MipChain.h"
#include "TextureShape.h"

#include <cmath>
#include <cstring>

namespace eacp::GPU
{
namespace
{
// Deferred: a command buffer still recording may name these handles.
void retireImage(VulkanContext& context,
                 VkImage image,
                 VmaAllocation allocation,
                 VkImageView first,
                 VkImageView second)
{
    if (image == VK_NULL_HANDLE)
        return;

    context.deferRelease(
        [allocator = context.getAllocator(), image, allocation, first, second]
        {
            for (auto view: {first, second})
                getVulkanShared().destroyImageView(view);

            // A swapchain image has no allocation of its own.
            if (allocation != nullptr)
                vmaDestroyImage(allocator, image, allocation);
        });
}

bool makeDepthImage(VulkanContext& context,
                    const VulkanTextureData& data,
                    bool sampleable,
                    int samples,
                    VkImage& image,
                    VmaAllocation& allocation)
{
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = data.depthFormat;
    info.extent = {static_cast<std::uint32_t>(data.width),
                   static_cast<std::uint32_t>(data.height),
                   1};

    // The colour image's levels and slices, so a pass into any level or layer
    // of it finds a depth plane of the same size there. A multisampled target
    // and a resolve have one of each.
    const auto single = samples > 1 || data.sampleCount > 1;
    info.mipLevels = single ? 1u : static_cast<std::uint32_t>(data.mipLevels);
    info.arrayLayers = single ? 1u : static_cast<std::uint32_t>(data.slices);
    info.samples = toVkSampleCount(samples);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    // So the read view can be the cube the colour image is.
    if (!single
        && (data.kind == TextureType::Cube || data.kind == TextureType::CubeArray))
        info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (sampleable)
        info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

    return vmaCreateImage(context.getAllocator(),
                          &info,
                          &allocationInfo,
                          &image,
                          &allocation,
                          nullptr)
           == VK_SUCCESS;
}
} // namespace

VkImageView makeVulkanImageView(VkDevice device,
                                VkImage image,
                                VkFormat viewFormat,
                                VkImageViewType type,
                                VkImageAspectFlags aspect,
                                int levels,
                                int layers)
{
    return makeVulkanImageView(
        device, image, viewFormat, type, aspect, 0, levels, 0, layers);
}

VkImageView makeVulkanImageView(VkDevice device,
                                VkImage image,
                                VkFormat viewFormat,
                                VkImageViewType type,
                                VkImageAspectFlags aspect,
                                int baseLevel,
                                int levels,
                                int baseLayer,
                                int layers)
{
    VkImageViewCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = type;
    info.format = viewFormat;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.baseMipLevel = static_cast<std::uint32_t>(baseLevel);
    info.subresourceRange.levelCount = static_cast<std::uint32_t>(levels);
    info.subresourceRange.baseArrayLayer = static_cast<std::uint32_t>(baseLayer);
    info.subresourceRange.layerCount = static_cast<std::uint32_t>(layers);

    auto view = VkImageView {VK_NULL_HANDLE};

    if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    return view;
}

// No TRANSIENT_ATTACHMENT: a second pass into the same target has to find the
// samples the first one wrote, and a transient attachment may discard them.
bool createVulkanMultisampleCompanion(VulkanContext& context,
                                      VulkanTextureData& data)
{
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = data.format;
    info.extent = {static_cast<std::uint32_t>(data.width),
                   static_cast<std::uint32_t>(data.height),
                   1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = toVkSampleCount(data.sampleCount);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo = {};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(context.getAllocator(),
                       &info,
                       &allocationInfo,
                       &data.msaaImage,
                       &data.msaaAllocation,
                       nullptr)
        != VK_SUCCESS)
    {
        data.msaaImage = VK_NULL_HANDLE;
        return false;
    }

    data.msaaView = makeVulkanImageView(context.getDevice(),
                                        data.msaaImage,
                                        data.format,
                                        VK_IMAGE_VIEW_TYPE_2D,
                                        VK_IMAGE_ASPECT_COLOR_BIT,
                                        1,
                                        1);

    return data.msaaView != VK_NULL_HANDLE;
}

VkImageViewType sampledViewType(TextureType kind);

void createVulkanDepthCompanion(VulkanContext& context,
                                VulkanTextureData& data,
                                bool withStencil,
                                bool sampleable)
{
    data.depthHasStencil = withStencil;
    data.depthFormat = depthAttachmentFormat(withStencil);

    const auto multisampled = data.sampleCount > 1;

    if (!makeDepthImage(context,
                        data,
                        sampleable && !multisampled,
                        data.sampleCount,
                        data.depthImage,
                        data.depthAllocation))
    {
        data.depthImage = VK_NULL_HANDLE;
        return;
    }

    data.depthAttachmentView = makeVulkanImageView(context.getDevice(),
                                                   data.depthImage,
                                                   data.depthFormat,
                                                   VK_IMAGE_VIEW_TYPE_2D,
                                                   depthAspectMask(withStencil),
                                                   1,
                                                   1);

    if (data.depthAttachmentView == VK_NULL_HANDLE || !sampleable)
        return;

    if (multisampled)
    {
        if (!makeDepthImage(context,
                            data,
                            true,
                            1,
                            data.resolvedDepthImage,
                            data.resolvedDepthAllocation))
        {
            data.resolvedDepthImage = VK_NULL_HANDLE;
            return;
        }

        data.resolvedDepthAttachmentView =
            makeVulkanImageView(context.getDevice(),
                                data.resolvedDepthImage,
                                data.depthFormat,
                                VK_IMAGE_VIEW_TYPE_2D,
                                depthAspectMask(withStencil),
                                1,
                                1);

        if (data.resolvedDepthAttachmentView == VK_NULL_HANDLE)
            return;
    }

    // Depth alone: Vulkan refuses a sampled view over two aspects. Level 0 of
    // every layer, as the kind's own view type - a 2D array, a cube, a cube
    // array - so the plane reads as Metal's and D3D12's do; a multisampled
    // target's resolve has the one layer.
    const auto whole = !multisampled && data.kind != TextureType::TwoD;

    data.depthReadView = makeVulkanImageView(context.getDevice(),
                                             data.sampledDepthImage(),
                                             data.depthFormat,
                                             whole ? sampledViewType(data.kind)
                                                   : VK_IMAGE_VIEW_TYPE_2D,
                                             VK_IMAGE_ASPECT_DEPTH_BIT,
                                             1,
                                             whole ? data.slices : 1);
}

void releaseVulkanCompanions(VulkanContext& context, VulkanTextureData& data)
{
    retireImage(
        context, data.msaaImage, data.msaaAllocation, data.msaaView, VK_NULL_HANDLE);

    const auto readsTheResolve = data.resolvedDepthImage != VK_NULL_HANDLE;

    retireImage(context,
                data.depthImage,
                data.depthAllocation,
                data.depthAttachmentView,
                readsTheResolve ? VK_NULL_HANDLE : data.depthReadView);
    retireImage(context,
                data.resolvedDepthImage,
                data.resolvedDepthAllocation,
                data.resolvedDepthAttachmentView,
                readsTheResolve ? data.depthReadView : VK_NULL_HANDLE);

    data.msaaImage = VK_NULL_HANDLE;
    data.msaaAllocation = nullptr;
    data.msaaView = VK_NULL_HANDLE;
    data.msaaUse = {};

    data.depthImage = VK_NULL_HANDLE;
    data.depthAllocation = nullptr;
    data.depthAttachmentView = VK_NULL_HANDLE;
    data.depthUse = {};

    data.resolvedDepthImage = VK_NULL_HANDLE;
    data.resolvedDepthAllocation = nullptr;
    data.resolvedDepthAttachmentView = VK_NULL_HANDLE;
    data.resolvedDepthUse = {};

    data.depthReadView = VK_NULL_HANDLE;
}

VkImageViewType sampledViewType(TextureType kind)
{
    switch (kind)
    {
        case TextureType::TwoD:
            return VK_IMAGE_VIEW_TYPE_2D;
        case TextureType::Cube:
            return VK_IMAGE_VIEW_TYPE_CUBE;
        case TextureType::TwoDArray:
            return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureType::CubeArray:
            return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case TextureType::ThreeD:
            return VK_IMAGE_VIEW_TYPE_3D;
    }

    return VK_IMAGE_VIEW_TYPE_2D;
}

// What a kernel writes: a 2D image, a cube or an array as a 2D array of its
// slices, a volume as itself.
VkImageViewType storageViewType(TextureType kind)
{
    if (kind == TextureType::TwoD)
        return VK_IMAGE_VIEW_TYPE_2D;

    return kind == TextureType::ThreeD ? VK_IMAGE_VIEW_TYPE_3D
                                       : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
}

namespace
{
VkImageView findSubresourceView(const VulkanTextureData& data,
                                VulkanSubresourceView::Use use,
                                int level,
                                int slice,
                                int count)
{
    for (const auto& each: data.subresourceViews)
        if (each.use == use && each.level == level && each.slice == slice
            && each.count == count)
            return each.view;

    return VK_NULL_HANDLE;
}

VkImageView keepSubresourceView(VulkanTextureData& data,
                                VulkanSubresourceView::Use use,
                                int level,
                                int slice,
                                int count,
                                VkImageView view)
{
    if (view != VK_NULL_HANDLE)
        data.subresourceViews.add({use, level, slice, count, view});

    return view;
}

bool isLevelZeroOfSliceZero(int level, int slice, int count)
{
    return level == 0 && slice == 0 && count == 1;
}

VkImageView makeTargetView(
    VulkanTextureData& data, bool depth, int level, int slice, int count)
{
    using Use = VulkanSubresourceView::Use;
    const auto use = depth ? Use::DepthAttachment : Use::ColorAttachment;

    if (auto view = findSubresourceView(data, use, level, slice, count))
        return view;

    const auto image = depth ? data.depthImage : data.image;

    if (image == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    const auto view = makeVulkanImageView(
        getVulkanShared().getDevice(),
        image,
        depth ? data.depthFormat : data.format,
        count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        depth ? depthAspectMask(data.depthHasStencil) : VK_IMAGE_ASPECT_COLOR_BIT,
        level,
        1,
        slice,
        count);

    return keepSubresourceView(data, use, level, slice, count, view);
}
} // namespace

VkImageView
    vulkanColorTargetView(VulkanTextureData& data, int level, int slice, int count)
{
    if (isLevelZeroOfSliceZero(level, slice, count) || data.isMultisampled())
        return data.colorAttachmentView();

    return makeTargetView(data, false, level, slice, count);
}

VkImageView
    vulkanDepthTargetView(VulkanTextureData& data, int level, int slice, int count)
{
    if (!data.hasDepth())
        return VK_NULL_HANDLE;

    if (isLevelZeroOfSliceZero(level, slice, count) || data.isMultisampled())
        return data.depthAttachmentView;

    return makeTargetView(data, true, level, slice, count);
}

VkImageView vulkanLevelView(VulkanTextureData& data, bool storage, int level)
{
    using Use = VulkanSubresourceView::Use;

    if (level < 0 || level >= data.mipLevels)
        return VK_NULL_HANDLE;

    if (data.mipLevels == 1)
        return storage ? data.storageView : data.sampledView;

    const auto use = storage ? Use::Storage : Use::Sampled;

    if (auto view = findSubresourceView(data, use, level, 0, data.slices))
        return view;

    const auto view = makeVulkanImageView(getVulkanShared().getDevice(),
                                          data.image,
                                          data.format,
                                          storage ? storageViewType(data.kind)
                                                  : sampledViewType(data.kind),
                                          VK_IMAGE_ASPECT_COLOR_BIT,
                                          level,
                                          1,
                                          0,
                                          data.slices);

    return keepSubresourceView(data, use, level, 0, data.slices, view);
}

VkImageView vulkanSampledArrayView(VulkanTextureData& data, int level)
{
    using Use = VulkanSubresourceView::Use;

    if (level < 0 || level >= data.mipLevels
        || (data.kind != TextureType::Cube && data.kind != TextureType::CubeArray))
        return VK_NULL_HANDLE;

    if (auto view =
            findSubresourceView(data, Use::SampledArray, level, 0, data.slices))
        return view;

    const auto view = makeVulkanImageView(getVulkanShared().getDevice(),
                                          data.image,
                                          data.format,
                                          VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                          VK_IMAGE_ASPECT_COLOR_BIT,
                                          level,
                                          1,
                                          0,
                                          data.slices);

    return keepSubresourceView(data, Use::SampledArray, level, 0, data.slices, view);
}

VkImageView vulkanDepthSliceView(VulkanTextureData& data, int slice)
{
    using Use = VulkanSubresourceView::Use;

    if (!data.hasSampleableDepth() || slice < 0)
        return VK_NULL_HANDLE;

    if (data.kind == TextureType::TwoD || data.isMultisampled())
        return slice == 0 ? data.depthReadView : VK_NULL_HANDLE;

    if (slice >= data.slices)
        return VK_NULL_HANDLE;

    if (auto view = findSubresourceView(data, Use::DepthSlice, 0, slice, 1))
        return view;

    const auto view = makeVulkanImageView(getVulkanShared().getDevice(),
                                          data.sampledDepthImage(),
                                          data.depthFormat,
                                          VK_IMAGE_VIEW_TYPE_2D,
                                          VK_IMAGE_ASPECT_DEPTH_BIT,
                                          0,
                                          1,
                                          slice,
                                          1);

    return keepSubresourceView(data, Use::DepthSlice, 0, slice, 1, view);
}

void releaseVulkanSubresourceViews(VulkanContext& context, VulkanTextureData& data)
{
    if (data.subresourceViews.empty())
        return;

    context.deferRelease(
        [views = data.subresourceViews]
        {
            for (const auto& each: views)
                getVulkanShared().destroyImageView(each.view);
        });

    data.subresourceViews.clear();
}

struct Texture::Native
{
    Native(Device& device,
           const TextureDescriptor& descriptor,
           const void* pixels,
           InitialData initialData)
        : context(getVulkanContext(device))
        , format(descriptor.format)
    {
        data.width = descriptor.width;
        data.height = descriptor.height;
        data.format = toVkFormat(descriptor.format);

        if (!context.isValid())
            return;

        const auto resolved = resolveTextureShape(descriptor, initialData);

        if (!resolved)
            return;

        shape = *resolved;
        data.kind = shape.kind;
        data.slices = shape.slices();
        data.depth = shape.depth;
        data.mipLevels = shape.levels;

        if (!descriptorIsPossible(device, descriptor))
            return;

        if (!createImage(descriptor) || !createViews(descriptor))
        {
            release();
            return;
        }

        const auto ready =
            pixels != nullptr ? upload(pixels, 0) : settleAtRestingLayout();

        if (!ready)
        {
            release();
            return;
        }

        if (data.sampleCount > 1 && !createVulkanMultisampleCompanion(context, data))
        {
            release();
            return;
        }

        if (descriptor.renderTarget
            && (descriptor.depth || descriptor.stencil
                || descriptor.sampleableDepth))
            createVulkanDepthCompanion(
                context, data, descriptor.stencil, descriptor.sampleableDepth);
    }

    Native(Device& device, void*)
        : context(getVulkanContext(device))
    {
    }

    ~Native() { release(); }

    bool descriptorIsPossible(Device& device, const TextureDescriptor& descriptor)
    {
        if (shape.kind == TextureType::CubeArray && !device.supportsCubeArrays())
        {
            LOG("Vulkan: the device has no imageCubeArray, so a cube array is "
                "invalid here");
            return false;
        }

        if (descriptor.renderTarget && descriptor.sampleCount > 1)
        {
            if (!device.supportsSampleCount(descriptor.sampleCount))
            {
                LOG("Vulkan: the device refuses ",
                    descriptor.sampleCount,
                    " samples, so the target is invalid rather than drawn at "
                    "a count its pipelines do not carry");
                return false;
            }

            if (descriptor.sampleableDepth
                && !getVulkanShared().resolvesDepthBySampleZero())
            {
                LOG("Vulkan: the device resolves no depth by sample zero, so a "
                    "multisampled sampleable-depth target is invalid rather "
                    "than sampled from a resolve that never ran");
                return false;
            }

            data.sampleCount = descriptor.sampleCount;
        }

        // See the block formats and RGB9E5Float in Texture.h: sampled only.
        if (!isRenderableFormat(format)
            && (descriptor.renderTarget || descriptor.computeWrite))
            return false;

        if (isCompressedFormat(format))
        {
            if (!device.supportsBlockCompression())
            {
                LOG("Vulkan: the device has no block-compressed formats, so a "
                    "compressed texture is invalid here");
                return false;
            }
        }

        if (descriptor.computeWrite)
        {
            // Asked rather than assumed: dropped stores are far harder to
            // find than a texture that refused to be created.
            if (!device.supportsComputeWrite(descriptor.format))
            {
                if (supportsComputeWrite(descriptor.format))
                    LOG("Vulkan: the device reports no storage-image support for "
                        "this format, so a computeWrite texture is invalid rather "
                        "than silently unwritable");

                return false;
            }

            computeWrite = true;
        }

        return true;
    }

    bool createImage(const TextureDescriptor& descriptor)
    {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = shape.isVolume() ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        info.format = data.format;
        info.extent = {static_cast<std::uint32_t>(data.width),
                       static_cast<std::uint32_t>(data.height),
                       static_cast<std::uint32_t>(shape.depth)};
        info.mipLevels = static_cast<std::uint32_t>(data.mipLevels);

        // Vulkan has no cube image type, only a cube view: six layers in the
        // order +X, -X, +Y, -Y, +Z, -Z, a cube array layers of six.
        info.arrayLayers = static_cast<std::uint32_t>(data.slices);
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                     | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        if (descriptor.renderTarget)
            info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        if (computeWrite)
            info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

        if (shape.isCubeKind())
            info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

        // A pass renders into one image of a volume, or a run of them, through
        // a 2D or 2D array view of its z range.
        if (shape.isVolume() && descriptor.renderTarget)
            info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;

        VmaAllocationCreateInfo allocationInfo = {};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

        if (vmaCreateImage(context.getAllocator(),
                           &info,
                           &allocationInfo,
                           &data.image,
                           &data.allocation,
                           nullptr)
            != VK_SUCCESS)
        {
            LOG("Vulkan: the device would not create a ",
                data.width,
                "x",
                data.height,
                " image in this format");

            data.image = VK_NULL_HANDLE;
            return false;
        }

        return true;
    }

    VkImageView makeView(VkImage image,
                         VkFormat viewFormat,
                         VkImageViewType type,
                         VkImageAspectFlags aspect,
                         int levels,
                         int layers)
    {
        return makeVulkanImageView(
            context.getDevice(), image, viewFormat, type, aspect, levels, layers);
    }

    bool createViews(const TextureDescriptor& descriptor)
    {
        data.sampledView = makeView(data.image,
                                    data.format,
                                    sampledViewType(shape.kind),
                                    VK_IMAGE_ASPECT_COLOR_BIT,
                                    data.mipLevels,
                                    data.slices);

        if (data.sampledView == VK_NULL_HANDLE)
            return false;

        if (descriptor.renderTarget)
        {
            data.attachmentView = makeView(data.image,
                                           data.format,
                                           VK_IMAGE_VIEW_TYPE_2D,
                                           VK_IMAGE_ASPECT_COLOR_BIT,
                                           1,
                                           1);

            if (data.attachmentView == VK_NULL_HANDLE)
                return false;
        }

        if (computeWrite)
        {
            data.storageView = makeView(data.image,
                                        data.format,
                                        storageViewType(shape.kind),
                                        VK_IMAGE_ASPECT_COLOR_BIT,
                                        1,
                                        data.slices);

            if (data.storageView == VK_NULL_HANDLE)
                return false;
        }

        return true;
    }

    void release()
    {
        context.cancelImageSettle(data);

        retireImage(context,
                    data.image,
                    data.allocation,
                    data.sampledView,
                    data.attachmentView);

        if (data.storageView != VK_NULL_HANDLE)
            context.deferRelease(
                [device = context.getDevice(), view = data.storageView]
                { vkDestroyImageView(device, view, nullptr); });

        releaseVulkanSubresourceViews(context, data);
        releaseVulkanCompanions(context, data);

        data = {};
    }

    // A discarded recording never runs, so the layout tracking it advanced has
    // to go back or the next barrier names a layout the image was never in.
    template <typename Record>
    bool onARecording(Record&& record)
    {
        auto* commands = context.getRecordingForCopy();
        const auto ownsRecording = commands == nullptr;

        if (ownsRecording)
            commands = context.acquire();

        if (commands == nullptr)
            return false;

        const auto tracked = data.use;
        const auto recorded = record(*commands);

        if (!ownsRecording)
            return recorded;

        if (recorded)
        {
            context.submit(commands);
            return true;
        }

        data.use = tracked;
        context.discard(commands);
        return false;
    }

    // Repacked tight: the caller's stride is in bytes where a bufferRowLength
    // is in texels. A row is a row of blocks on a compressed format.
    bool copyPixels(CommandContext& commands, const SubresourceUpload& upload)
    {
        const auto rowBytes =
            static_cast<std::size_t>(levelBytesPerRow(format, upload.width));
        const auto rows = static_cast<std::size_t>(levelRows(format, upload.height));
        const auto images = static_cast<std::size_t>(upload.depth);

        auto source = context.allocateUpload(commands, rowBytes * rows * images);

        if (!source.isValid())
            return false;

        const auto* in = static_cast<const std::byte*>(upload.pixels);

        for (auto image = std::size_t {0}; image < images; ++image)
            for (auto row = std::size_t {0}; row < rows; ++row)
                std::memcpy(
                    source.mapped + (image * rows + row) * rowBytes,
                    in + image * static_cast<std::size_t>(upload.bytesPerImage)
                        + row * static_cast<std::size_t>(upload.bytesPerRow),
                    rowBytes);

        VkBufferImageCopy region = {};
        region.bufferOffset = source.offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = static_cast<std::uint32_t>(upload.level);
        region.imageSubresource.baseArrayLayer =
            static_cast<std::uint32_t>(upload.slice);
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {upload.x, upload.y, upload.z};
        region.imageExtent = {static_cast<std::uint32_t>(upload.width),
                              static_cast<std::uint32_t>(upload.height),
                              static_cast<std::uint32_t>(upload.depth)};

        vkCmdCopyBufferToImage(commands.buffer,
                               source.buffer,
                               data.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &region);
        return true;
    }

    bool submitUploads(const Vector<SubresourceUpload>& uploads)
    {
        if (!data.isValid() || uploads.empty())
            return false;

        return onARecording(
            [&](CommandContext& commands)
            {
                transitionTextureForUse(commands.buffer, data, imageTransferDst);

                for (const auto& each: uploads)
                    if (!copyPixels(commands, each))
                        return false;

                transitionTextureForUse(commands.buffer, data, data.restingUse());
                return true;
            });
    }

    bool upload(const void* pixels, int bytesPerRow)
    {
        const auto plan = planWholeUpload(shape, format, pixels, bytesPerRow);
        return plan.valid && submitUploads(plan.uploads);
    }

    // UNDEFINED is not a layout anything may be bound at, and an image no
    // upload wrote is still in it, so it has to be moved to its resting layout.
    // With no recording open the context queues the barrier instead, and a
    // burst of pixel-less textures then costs one submission rather than one
    // each; with one open it joins that recording as an upload would.
    bool settleAtRestingLayout()
    {
        if (context.getOpenRecording() == nullptr)
        {
            context.deferImageSettle(data);
            return true;
        }

        return onARecording(
            [&](CommandContext& commands)
            {
                transitionTextureForUse(commands.buffer, data, data.restingUse());
                return true;
            });
    }

    void update(const void* pixels, int bytesPerRow)
    {
        if (data.isValid())
            upload(pixels, bytesPerRow);
    }

    // The two old region forms reach level 0 of an uncompressed 2D texture
    // alone, in the same words the Metal backend uses - see the header.
    bool takesLevelZeroRegions() const
    {
        return shape.kind == TextureType::TwoD && !isCompressedFormat(format);
    }

    static TextureRegion levelZeroRegion(
        int x, int y, int regionWidth, int regionHeight, int bytesPerRow)
    {
        auto region = TextureRegion {};
        region.origin = {x, y, 0};
        region.size = {regionWidth, regionHeight, 1};
        region.bytesPerRow = bytesPerRow;
        return region;
    }

    void updateRegion(int x,
                      int y,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      int bytesPerRow)
    {
        if (regionWidth <= 0 || regionHeight <= 0 || !takesLevelZeroRegions())
            return;

        update(levelZeroRegion(x, y, regionWidth, regionHeight, bytesPerRow),
               pixels);
    }

    void update(const TextureRegion& region, const void* pixels)
    {
        if (auto upload = planRegionUpload(shape, format, region, pixels))
            submitUploads(Vector<SubresourceUpload> {*upload});
    }

    void readRegion(int x,
                    int y,
                    int regionWidth,
                    int regionHeight,
                    void* dst,
                    int bytesPerRow) const
    {
        if (regionWidth <= 0 || regionHeight <= 0 || !takesLevelZeroRegions())
            return;

        read(levelZeroRegion(x, y, regionWidth, regionHeight, bytesPerRow), dst);
    }

    // A recording of its own: an open one cannot be waited on.
    void read(const TextureRegion& region, void* dst) const
    {
        if (!data.isValid() || !context.isValid())
            return;

        const auto plan = planRegionRead(shape, format, region, dst);

        if (!plan)
            return;

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        const auto rowBytes =
            static_cast<std::size_t>(levelBytesPerRow(format, plan->width));
        const auto rows = static_cast<std::size_t>(plan->height);
        const auto images = static_cast<std::size_t>(plan->depth);
        const auto bytes = rowBytes * rows * images;

        std::byte* mapped = nullptr;
        auto staging = context.acquireReadbackBuffer(*commands, bytes, mapped);

        if (staging == VK_NULL_HANDLE || mapped == nullptr)
        {
            context.discard(commands);
            return;
        }

        transitionTextureForUse(commands->buffer, data, imageTransferSrc);

        VkBufferImageCopy copy = {};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = static_cast<std::uint32_t>(plan->level);
        copy.imageSubresource.baseArrayLayer =
            static_cast<std::uint32_t>(plan->slice);
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {plan->x, plan->y, plan->z};
        copy.imageExtent = {static_cast<std::uint32_t>(plan->width),
                            static_cast<std::uint32_t>(plan->height),
                            static_cast<std::uint32_t>(plan->depth)};

        vkCmdCopyImageToBuffer(commands->buffer,
                               data.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging,
                               1,
                               &copy);

        transitionTextureForUse(commands->buffer, data, data.restingUse());

        context.waitFor(context.submit(commands));

        // Coherent memory, so no invalidate before the CPU reads.
        auto* out = static_cast<std::byte*>(dst);

        for (auto image = std::size_t {0}; image < images; ++image)
            for (auto row = std::size_t {0}; row < rows; ++row)
                std::memcpy(
                    out + image * static_cast<std::size_t>(plan->bytesPerImage)
                        + row * static_cast<std::size_t>(plan->bytesPerRow),
                    mapped + (image * rows + row) * rowBytes,
                    rowBytes);
    }

    VulkanContext& context;

    TextureFormat format = TextureFormat::RGBA8Unorm;

    TextureShape shape;

    bool computeWrite = false;

    mutable VulkanTextureData data;
};

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 const void* pixels)
    : impl(device,
           descriptor,
           pixels,
           pixels != nullptr ? InitialData::Pixels : InitialData::None)
{
}

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 std::span<const TextureSubresource> subresources)
    : impl(device, descriptor, nullptr, InitialData::Subresources)
{
    if (!isValid())
        return;

    const auto plan =
        planSubresourceUploads(impl->shape, impl->format, subresources);

    if (!plan.valid)
    {
        impl->release();
        return;
    }

    impl->submitUploads(plan.uploads);
}

Texture::Texture(Device& device, void* nativePixelBuffer)
    : impl(device, nativePixelBuffer)
{
}

void Texture::update(const void* pixels, int bytesPerRow)
{
    if (bytesPerRow < 0)
        return;

    impl->update(pixels, bytesPerRow);
}

void Texture::update(const Graphics::Rect& region,
                     const void* pixels,
                     int bytesPerRow)
{
    if (bytesPerRow < 0)
        return;

    impl->updateRegion(static_cast<int>(std::lround(region.x)),
                       static_cast<int>(std::lround(region.y)),
                       static_cast<int>(std::lround(region.w)),
                       static_cast<int>(std::lround(region.h)),
                       pixels,
                       bytesPerRow);
}

void Texture::updateSubresource(const TextureRegion& region, const void* pixels)
{
    impl->update(region, pixels);
}

void Texture::read(void* dst, int bytesPerRow) const
{
    if (bytesPerRow < 0)
        return;

    impl->readRegion(0, 0, impl->data.width, impl->data.height, dst, bytesPerRow);
}

void Texture::read(const Graphics::Rect& region, void* dst, int bytesPerRow) const
{
    if (bytesPerRow < 0)
        return;

    impl->readRegion(static_cast<int>(std::lround(region.x)),
                     static_cast<int>(std::lround(region.y)),
                     static_cast<int>(std::lround(region.w)),
                     static_cast<int>(std::lround(region.h)),
                     dst,
                     bytesPerRow);
}

void Texture::readSubresource(const TextureRegion& region, void* dst) const
{
    impl->read(region, dst);
}

TextureType Texture::kind() const
{
    return impl->shape.kind;
}

int Texture::arrayLength() const
{
    return impl->shape.layers;
}

int Texture::volumeDepth() const
{
    return impl->shape.depth;
}

int Texture::slices() const
{
    return impl->shape.slices();
}

void* Texture::nativeSubresourceView(int, int, int, int) const
{
    return nullptr;
}

void* Texture::nativeLevelView(int) const
{
    return nullptr;
}

void* Texture::nativeArrayView(int) const
{
    return nullptr;
}

void* Texture::nativeDepthSliceView(int) const
{
    return nullptr;
}

int Texture::width() const
{
    return impl->data.width;
}

int Texture::height() const
{
    return impl->data.height;
}

TextureFormat Texture::format() const
{
    return impl->format;
}

bool Texture::isValid() const
{
    return impl->data.isValid();
}

int Texture::mipLevels() const
{
    return impl->data.mipLevels;
}

bool Texture::isRenderTarget() const
{
    return isValid() && impl->data.isRenderTarget();
}

bool Texture::isCube() const
{
    return isValid() && impl->shape.kind == TextureType::Cube;
}

bool Texture::isComputeWritable() const
{
    return isValid() && impl->data.isComputeWritable();
}

bool Texture::hasDepth() const
{
    return isValid() && impl->data.hasDepth();
}

bool Texture::hasStencil() const
{
    return isValid() && impl->data.hasStencil();
}

bool Texture::hasSampleableDepth() const
{
    return isValid() && impl->data.hasSampleableDepth();
}

int Texture::sampleCount() const
{
    return isValid() ? impl->data.sampleCount : 1;
}

void* Texture::nativeTexture() const
{
    return &impl->data;
}

void* Texture::nativeReadView() const
{
    return &impl->data;
}

void* Texture::nativeDepthTexture() const
{
    return nullptr;
}

void* Texture::nativeMultisampleTexture() const
{
    return nullptr;
}

void* Texture::nativeResolvedDepthTexture() const
{
    return nullptr;
}
} // namespace eacp::GPU
