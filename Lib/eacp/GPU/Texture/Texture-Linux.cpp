#include "Texture.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"
#include "MipChain.h"

#include <cmath>
#include <cstring>

namespace eacp::GPU
{
namespace
{
// Asked rather than assumed: dropped stores are far harder to find than a
// texture that refused to be created.
bool supportsStorageImage(VkFormat format)
{
    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(
        getVulkanShared().getPhysicalDevice(), format, &properties);

    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
           != 0;
}

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
        [allocator = context.getAllocator(),
         device = context.getDevice(),
         image,
         allocation,
         first,
         second]
        {
            for (auto view: {first, second})
                if (view != VK_NULL_HANDLE)
                    vkDestroyImageView(device, view, nullptr);

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
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = toVkSampleCount(samples);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
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
    VkImageViewCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = type;
    info.format = viewFormat;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.levelCount = static_cast<std::uint32_t>(levels);
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

    // Depth alone: Vulkan refuses a sampled view over two aspects.
    data.depthReadView = makeVulkanImageView(context.getDevice(),
                                             data.sampledDepthImage(),
                                             data.depthFormat,
                                             VK_IMAGE_VIEW_TYPE_2D,
                                             VK_IMAGE_ASPECT_DEPTH_BIT,
                                             1,
                                             1);
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

struct Texture::Native
{
    Native(Device& device, const TextureDescriptor& descriptor, const void* pixels)
        : context(getVulkanContext(device))
        , format(descriptor.format)
    {
        data.width = descriptor.width;
        data.height = descriptor.height;
        data.cube = descriptor.cube;
        data.format = toVkFormat(descriptor.format);

        if (!context.isValid() || data.width <= 0 || data.height <= 0)
            return;

        if (!descriptorIsPossible(device, descriptor, pixels))
            return;

        if (!createImage(descriptor) || !createViews(descriptor))
        {
            release();
            return;
        }

        if (pixels != nullptr && !upload(pixels, 0))
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

    bool descriptorIsPossible(Device& device,
                              const TextureDescriptor& descriptor,
                              const void* pixels)
    {
        if (data.cube
            && (data.width != data.height || descriptor.renderTarget
                || descriptor.computeWrite))
            return false;

        if (descriptor.renderTarget && descriptor.sampleCount > 1)
        {
            if (data.cube)
                return false;

            if (!device.supportsSampleCount(descriptor.sampleCount))
            {
                LOG("Vulkan: the device refuses ",
                    descriptor.sampleCount,
                    " samples, so the target is invalid rather than drawn at "
                    "a count its pipelines do not carry");
                return false;
            }

            data.sampleCount = descriptor.sampleCount;
        }

        if (isCompressedFormat(format))
        {
            if (descriptor.renderTarget || descriptor.computeWrite)
                return false;

            if (!device.supportsBlockCompression())
            {
                LOG("Vulkan: the device has no block-compressed formats, so a "
                    "compressed texture is invalid here");
                return false;
            }
        }

        if (descriptor.computeWrite)
        {
            if (!supportsComputeWrite(descriptor.format))
                return false;

            if (!supportsStorageImage(data.format))
            {
                LOG("Vulkan: the device reports no storage-image support for "
                    "this format, so a computeWrite texture is invalid rather "
                    "than silently unwritable");
                return false;
            }

            computeWrite = true;
        }

        if (descriptor.mipLevels < 0)
            return false;

        if (descriptor.mipLevels > 0)
        {
            if (descriptor.mipmapped || descriptor.renderTarget
                || descriptor.computeWrite || data.cube || pixels == nullptr
                || descriptor.mipLevels > mipLevelCount(data.width, data.height))
                return false;

            data.mipLevels = descriptor.mipLevels;
            suppliedChain = true;
        }
        else if (descriptor.mipmapped && pixels != nullptr
                 && canBuildMipChain(format))
        {
            data.mipLevels = mipLevelCount(data.width, data.height);
        }

        return true;
    }

    bool createImage(const TextureDescriptor& descriptor)
    {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = data.format;
        info.extent = {static_cast<std::uint32_t>(data.width),
                       static_cast<std::uint32_t>(data.height),
                       1};
        info.mipLevels = static_cast<std::uint32_t>(data.mipLevels);

        // Vulkan has no cube image type, only a cube view: six layers in the
        // order +X, -X, +Y, -Y, +Z, -Z.
        info.arrayLayers = static_cast<std::uint32_t>(data.cube ? 6 : 1);
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

        if (data.cube)
            info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

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
        data.sampledView =
            makeView(data.image,
                     data.format,
                     data.cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
                     VK_IMAGE_ASPECT_COLOR_BIT,
                     data.mipLevels,
                     data.cube ? 6 : 1);

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
                                        VK_IMAGE_VIEW_TYPE_2D,
                                        VK_IMAGE_ASPECT_COLOR_BIT,
                                        1,
                                        1);

            if (data.storageView == VK_NULL_HANDLE)
                return false;
        }

        return true;
    }

    void release()
    {
        retireImage(context,
                    data.image,
                    data.allocation,
                    data.sampledView,
                    data.attachmentView);

        if (data.storageView != VK_NULL_HANDLE)
            context.deferRelease(
                [device = context.getDevice(), view = data.storageView]
                { vkDestroyImageView(device, view, nullptr); });

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
    bool copyPixels(CommandContext& commands,
                    const void* pixels,
                    int sourcePitch,
                    int destX,
                    int destY,
                    int regionWidth,
                    int regionHeight,
                    int mipLevel,
                    int face)
    {
        const auto rowBytes =
            static_cast<std::size_t>(levelBytesPerRow(format, regionWidth));
        const auto rows = levelRows(format, regionHeight);
        const auto pitch = static_cast<std::size_t>(sourcePitch);

        auto source = context.allocateUpload(
            commands, rowBytes * static_cast<std::size_t>(rows));

        if (!source.isValid())
            return false;

        const auto* in = static_cast<const std::byte*>(pixels);

        for (auto row = 0; row < rows; ++row)
            std::memcpy(source.mapped + static_cast<std::size_t>(row) * rowBytes,
                        in + static_cast<std::size_t>(row) * pitch,
                        rowBytes);

        VkBufferImageCopy region = {};
        region.bufferOffset = source.offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = static_cast<std::uint32_t>(mipLevel);
        region.imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(face);
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {destX, destY, 0};
        region.imageExtent = {static_cast<std::uint32_t>(regionWidth),
                              static_cast<std::uint32_t>(regionHeight),
                              1};

        vkCmdCopyBufferToImage(commands.buffer,
                               source.buffer,
                               data.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &region);
        return true;
    }

    bool recordLevels(CommandContext& commands, const void* pixels, int sourcePitch)
    {
        const auto faces = data.cube ? 6 : 1;
        const auto faceBytes =
            static_cast<std::size_t>(sourcePitch)
            * static_cast<std::size_t>(levelRows(format, data.height));

        for (auto face = 0; face < faces; ++face)
        {
            const auto* facePixels = static_cast<const std::byte*>(pixels)
                                     + static_cast<std::size_t>(face) * faceBytes;

            if (!recordFace(commands, facePixels, sourcePitch, face))
                return false;
        }

        return true;
    }

    bool recordFace(CommandContext& commands,
                    const void* pixels,
                    int sourcePitch,
                    int face)
    {
        if (data.mipLevels <= 1)
            return copyPixels(commands,
                              pixels,
                              sourcePitch,
                              0,
                              0,
                              data.width,
                              data.height,
                              0,
                              face);

        if (suppliedChain)
            return recordChain(commands, pixels, nullptr, face);

        const auto chain =
            buildMipChain(pixels, data.width, data.height, format, sourcePitch);

        if (!chain.isValid())
            return false;

        return recordChain(commands, nullptr, &chain, face);
    }

    bool recordChain(CommandContext& commands,
                     const void* packed,
                     const MipChain* chain,
                     int face)
    {
        const auto* bytes = static_cast<const std::byte*>(packed);

        for (auto level = 0; level < data.mipLevels; ++level)
        {
            const auto levelWidth = mipExtent(data.width, level);
            const auto levelHeight = mipExtent(data.height, level);

            if (!copyPixels(commands,
                            chain != nullptr ? chain->level(level) : bytes,
                            levelBytesPerRow(format, levelWidth),
                            0,
                            0,
                            levelWidth,
                            levelHeight,
                            level,
                            face))
                return false;

            if (bytes != nullptr)
                bytes += levelBytes(format, levelWidth, levelHeight);
        }

        return true;
    }

    bool upload(const void* pixels, int bytesPerRow)
    {
        const auto pitch =
            bytesPerRow != 0 ? bytesPerRow : levelBytesPerRow(format, data.width);

        return onARecording(
            [&](CommandContext& commands)
            {
                transitionTextureForUse(commands.buffer, data, imageTransferDst);

                if (!recordLevels(commands, pixels, pitch))
                    return false;

                transitionTextureForUse(commands.buffer, data, data.restingUse());
                return true;
            });
    }

    void update(const void* pixels, int bytesPerRow)
    {
        if (!data.isValid() || pixels == nullptr)
            return;

        if (bytesPerRow != 0 && (suppliedChain || isCompressedFormat(format)))
            return;

        if (data.mipLevels > 1 || data.cube || isCompressedFormat(format))
        {
            upload(pixels, bytesPerRow);
            return;
        }

        updateRegion(0, 0, data.width, data.height, pixels, bytesPerRow);
    }

    void updateRegion(int x,
                      int y,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      int bytesPerRow)
    {
        if (!data.isValid() || pixels == nullptr)
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        if (data.cube)
            return;

        // A compressed rect would have to land on the 4x4 block grid.
        if (isCompressedFormat(format))
            return;

        if (x < 0 || y < 0 || x + regionWidth > data.width
            || y + regionHeight > data.height)
            return;

        const auto pitch =
            bytesPerRow != 0 ? bytesPerRow : levelBytesPerRow(format, regionWidth);

        onARecording(
            [&](CommandContext& commands)
            {
                transitionTextureForUse(commands.buffer, data, imageTransferDst);

                if (!copyPixels(commands,
                                pixels,
                                pitch,
                                x,
                                y,
                                regionWidth,
                                regionHeight,
                                0,
                                0))
                    return false;

                transitionTextureForUse(commands.buffer, data, data.restingUse());
                return true;
            });
    }

    // A recording of its own: an open one cannot be waited on.
    void readRegion(int x,
                    int y,
                    int regionWidth,
                    int regionHeight,
                    void* dst,
                    int bytesPerRow) const
    {
        if (!data.isValid() || dst == nullptr || !context.isValid())
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        if (data.cube || isCompressedFormat(format))
            return;

        if (x < 0 || y < 0 || x + regionWidth > data.width
            || y + regionHeight > data.height)
            return;

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        const auto rowBytes =
            static_cast<std::size_t>(levelBytesPerRow(format, regionWidth));
        const auto bytes = rowBytes * static_cast<std::size_t>(regionHeight);

        std::byte* mapped = nullptr;
        auto staging = context.acquireReadbackBuffer(*commands, bytes, mapped);

        if (staging == VK_NULL_HANDLE || mapped == nullptr)
        {
            context.discard(commands);
            return;
        }

        transitionTextureForUse(commands->buffer, data, imageTransferSrc);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {x, y, 0};
        region.imageExtent = {static_cast<std::uint32_t>(regionWidth),
                              static_cast<std::uint32_t>(regionHeight),
                              1};

        vkCmdCopyImageToBuffer(commands->buffer,
                               data.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging,
                               1,
                               &region);

        transitionTextureForUse(commands->buffer, data, data.restingUse());

        context.waitFor(context.submit(commands));

        // Coherent memory, so no invalidate before the CPU reads.
        const auto stride =
            bytesPerRow != 0 ? static_cast<std::size_t>(bytesPerRow) : rowBytes;
        auto* out = static_cast<std::byte*>(dst);

        for (auto row = 0; row < regionHeight; ++row)
            std::memcpy(out + static_cast<std::size_t>(row) * stride,
                        mapped + static_cast<std::size_t>(row) * rowBytes,
                        rowBytes);
    }

    VulkanContext& context;

    TextureFormat format = TextureFormat::RGBA8Unorm;

    bool suppliedChain = false;

    bool computeWrite = false;

    mutable VulkanTextureData data;
};

Texture::Texture(Device& device,
                 const TextureDescriptor& descriptor,
                 const void* pixels)
    : impl(device, descriptor, pixels)
{
}

Texture::Texture(Device& device, void* nativePixelBuffer)
    : impl(device, nativePixelBuffer)
{
}

void Texture::update(const void* pixels, int bytesPerRow)
{
    impl->update(pixels, bytesPerRow);
}

void Texture::update(const Graphics::Rect& region,
                     const void* pixels,
                     int bytesPerRow)
{
    impl->updateRegion(static_cast<int>(std::lround(region.x)),
                       static_cast<int>(std::lround(region.y)),
                       static_cast<int>(std::lround(region.w)),
                       static_cast<int>(std::lround(region.h)),
                       pixels,
                       bytesPerRow);
}

void Texture::read(void* dst, int bytesPerRow) const
{
    impl->readRegion(0, 0, impl->data.width, impl->data.height, dst, bytesPerRow);
}

void Texture::read(const Graphics::Rect& region, void* dst, int bytesPerRow) const
{
    impl->readRegion(static_cast<int>(std::lround(region.x)),
                     static_cast<int>(std::lround(region.y)),
                     static_cast<int>(std::lround(region.w)),
                     static_cast<int>(std::lround(region.h)),
                     dst,
                     bytesPerRow);
}

int Texture::width() const
{
    return impl->data.width;
}

int Texture::height() const
{
    return impl->data.height;
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
    return isValid() && impl->data.cube;
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
