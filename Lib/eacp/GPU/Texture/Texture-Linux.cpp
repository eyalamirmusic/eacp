#include "Texture.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"
#include "MipChain.h"

#include <cmath>
#include <cstring>

// Linux/Vulkan backend. A texture is a VkImage allocated through VMA plus the
// views the three bind paths need: one over every level for a sampler, one over
// level 0 for a pass to attach, and one more for a kernel to write through. A
// render target grows the companions its descriptor asked for beside it - a
// multisampled colour image that passes actually render into, a depth image,
// and the single-sampled twin depth resolves into when a shader is going to
// read it - and every one of them lives exactly as long as the texture, so a
// render target stays one object with one lifetime.
//
// Uploads stage through the recording's upload arena and land on whatever
// recording is already open, which is Buffer's behaviour rather than the D3D12
// texture's: a glyph atlas taking a hundred region updates between two draws
// pays one submission for the frame instead of a hundred. read() is the
// exception and acquires a recording of its own, because it has to wait for
// what it copied and an open recording cannot be waited on.
//
// Layouts are tracked per image for the image's whole life (VulkanTextureData),
// there being no first-use-is-free rule for one: what the previous submission
// left an image in is what it is still in. Everything here leaves the texture
// in its resting layout - sampleable - so a pass, which cannot record a barrier
// once vkCmdBeginRendering has run, always knows where to move it from.

namespace eacp::GPU
{
namespace
{
// Whether the device will let a kernel store into this format. The formats
// supportsComputeWrite() allows are the ones both other backends guarantee, but
// a guarantee is not a driver, so it is asked rather than assumed - a kernel
// whose stores are silently dropped is a great deal harder to find than a
// texture that refused to be created.
bool supportsStorageImage(VkFormat format)
{
    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(
        getVulkanShared().getPhysicalDevice(), format, &properties);

    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
           != 0;
}
} // namespace

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

        // Created after the texture and refused with it: a pass rendering into
        // the single-sampled image where a multisampled one was asked for would
        // silently be a different pass.
        if (data.sampleCount > 1 && !createMultisampleTarget())
        {
            release();
            return;
        }

        // A failed depth buffer leaves a render target that can still be drawn
        // into without a depth test, which is what hasDepth() answers and what
        // every pass already branches on - the same answer the other two
        // backends give.
        if (descriptor.renderTarget
            && (descriptor.depth || descriptor.stencil
                || descriptor.sampleableDepth))
            createDepthBuffer(descriptor.stencil, descriptor.sampleableDepth);
    }

    // Zero-copy wrapping of a platform pixel buffer has nothing to wrap on
    // Linux: there is no camera or video backend to produce one. Invalid, as on
    // Windows, which the higher layer detects and falls back from by uploading
    // through update().
    Native(Device& device, void*)
        : context(getVulkanContext(device))
    {
    }

    ~Native() { release(); }

    // Everything the descriptor asks for that this device, or this library,
    // will not do. Refused before anything is created, so a texture that cannot
    // be what was asked for is invalid rather than quietly something else.
    //
    // **Only the answers a device gave are logged.** The rest are
    // contradictions within the descriptor itself, documented in Texture.h and
    // deliberately exercised by the tests that pin them - a line each would
    // bury the device's answer in a log full of expected refusals.
    bool descriptorIsPossible(Device& device,
                              const TextureDescriptor& descriptor,
                              const void* pixels)
    {
        // Six square faces, and nothing here can say which one a pass or a
        // kernel would write. Refused in the same words the other two backends
        // use - see TextureDescriptor::cube.
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

        // A block of bytes the sampler decodes and nothing else, so there is no
        // per-texel address for a pass or a kernel to write to.
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

        // A caller-supplied chain is taken as it is, and everything that would
        // make it a chain nobody could have supplied is refused rather than
        // reconciled - see TextureDescriptor::mipLevels, which lists the same
        // six the other two backends refuse.
        if (descriptor.mipLevels > 0)
        {
            if (descriptor.mipmapped || descriptor.renderTarget
                || descriptor.computeWrite || data.cube || pixels == nullptr
                || descriptor.mipLevels > mipLevelCount(data.width, data.height))
                return false;

            data.mipLevels = descriptor.mipLevels;
            suppliedChain = true;
        }
        // A chain eacp builds needs pixels to build it from and a format it can
        // average: a render target or a kernel output has neither at creation,
        // and a compressed one has no average, so either would get levels
        // nothing ever writes and the sampler would read them.
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

        // A cube is six array layers plus the flag that lets a view read them
        // as one - Vulkan has no cube image type, only a cube *view*, which is
        // the shape D3D12 has too, and the layer order is the same +X, -X, +Y,
        // -Y, +Z, -Z the Metal backend uploads in.
        info.arrayLayers = static_cast<std::uint32_t>(data.cube ? 6 : 1);
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // Sampled and copyable always: every texture here can be bound to a
        // shader, uploaded into and read back, and none of those costs anything
        // at creation. The two that do are asked for.
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
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image;
        info.viewType = type;
        info.format = viewFormat;
        info.subresourceRange.aspectMask = aspect;
        info.subresourceRange.levelCount = static_cast<std::uint32_t>(levels);
        info.subresourceRange.layerCount = static_cast<std::uint32_t>(layers);

        auto view = VkImageView {VK_NULL_HANDLE};

        if (vkCreateImageView(context.getDevice(), &info, nullptr, &view)
            != VK_SUCCESS)
            return VK_NULL_HANDLE;

        return view;
    }

    // **The view is what makes a cube a cube**, the image underneath being a
    // six-layer 2D array like any other. The attachment and storage views are
    // deliberately not that: a pass and a kernel each write one rectangle, and
    // level 0 of layer 0 is the only one either has a way to name.
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

    // The multisampled image a pass into this target actually renders into, and
    // which resolves into the texture at the end of every pass - so what a
    // shader samples, and what read() reads, is always the resolved picture.
    //
    // The samples are kept rather than discarded (no TRANSIENT_ATTACHMENT), for
    // the reason DepthAction::Resume exists: a second pass into the same target
    // has to find the samples the first one wrote, and a transient attachment
    // is allowed to throw them away the moment the resolve is done.
    bool createMultisampleTarget()
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

        data.msaaView = makeView(data.msaaImage,
                                 data.format,
                                 VK_IMAGE_VIEW_TYPE_2D,
                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                 1,
                                 1);

        return data.msaaView != VK_NULL_HANDLE;
    }

    bool makeDepthImage(bool sampleable,
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

    // The depth buffer a pass into this target attaches, at the target's own
    // sample count - both APIs require every attachment of one pass to agree on
    // it - and, where a shader is going to read the depth, the single-sampled
    // twin the attachment resolves into.
    //
    // **The resolve is the driver's own here**, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
    // through the depth attachment's resolve fields, which is exactly what
    // Metal's depth resolve does - so the two backends hand a shader the same
    // value, and this one needs neither D3D12's RESOLVE_MODE_MAX nor the shader
    // fallback its drivers made necessary.
    void createDepthBuffer(bool withStencil, bool sampleable)
    {
        data.depthHasStencil = withStencil;
        data.depthFormat = depthAttachmentFormat(withStencil);

        const auto multisampled = data.sampleCount > 1;

        // On a multisampled target the buffer a shader reads is the resolve
        // below, not the attachment, so the attachment pays nothing for
        // sampleableDepth there.
        if (!makeDepthImage(sampleable && !multisampled,
                            data.sampleCount,
                            data.depthImage,
                            data.depthAllocation))
        {
            data.depthImage = VK_NULL_HANDLE;
            return;
        }

        data.depthAttachmentView = makeView(data.depthImage,
                                            data.depthFormat,
                                            VK_IMAGE_VIEW_TYPE_2D,
                                            depthAspectMask(withStencil),
                                            1,
                                            1);

        if (data.depthAttachmentView == VK_NULL_HANDLE || !sampleable)
            return;

        if (multisampled)
        {
            if (!makeDepthImage(
                    true, 1, data.resolvedDepthImage, data.resolvedDepthAllocation))
            {
                data.resolvedDepthImage = VK_NULL_HANDLE;
                return;
            }

            data.resolvedDepthAttachmentView = makeView(data.resolvedDepthImage,
                                                        data.depthFormat,
                                                        VK_IMAGE_VIEW_TYPE_2D,
                                                        depthAspectMask(withStencil),
                                                        1,
                                                        1);

            if (data.resolvedDepthAttachmentView == VK_NULL_HANDLE)
                return;
        }

        // Depth alone, whatever the buffer carries: Vulkan refuses a sampled
        // view over two aspects, and a shader eacp generates reads the depth.
        data.depthReadView = makeView(data.sampledDepthImage(),
                                      data.depthFormat,
                                      VK_IMAGE_VIEW_TYPE_2D,
                                      VK_IMAGE_ASPECT_DEPTH_BIT,
                                      1,
                                      1);
    }

    // Handed to the context rather than destroyed here, on the same terms as a
    // buffer: a texture is routinely replaced mid-frame and the command buffer
    // still recording names the old handles.
    void release()
    {
        const auto retire = [this](VkImage image,
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

                    vmaDestroyImage(allocator, image, allocation);
                });
        };

        // The colour image owns three views, so it goes twice - the storage one
        // is null on everything that is not a kernel output, and a null view is
        // skipped rather than destroyed.
        retire(data.image, data.allocation, data.sampledView, data.attachmentView);

        if (data.storageView != VK_NULL_HANDLE)
            context.deferRelease(
                [device = context.getDevice(), view = data.storageView]
                { vkDestroyImageView(device, view, nullptr); });

        retire(data.msaaImage, data.msaaAllocation, data.msaaView, VK_NULL_HANDLE);

        // The read view goes with whichever image it was made over, which is
        // the resolve when there is one and the attachment otherwise.
        const auto readsTheResolve = data.resolvedDepthImage != VK_NULL_HANDLE;

        retire(data.depthImage,
               data.depthAllocation,
               data.depthAttachmentView,
               readsTheResolve ? VK_NULL_HANDLE : data.depthReadView);
        retire(data.resolvedDepthImage,
               data.resolvedDepthAllocation,
               data.resolvedDepthAttachmentView,
               readsTheResolve ? data.depthReadView : VK_NULL_HANDLE);

        data = {};
    }

    // -------------------------------------------------------------- uploading

    // Runs `record` on whatever recording is already open, and on one of this
    // context's own otherwise - the shape Buffer::stage has, and the reason a
    // texture uploaded to during a frame costs that frame no extra submission.
    // The one time the open recording will not take it is while a render pass
    // instance is running on it, where a copy and its barrier are both illegal;
    // see VulkanContext::getRecordingForCopy.
    //
    // **The tracked layout goes back where it was if nothing is submitted.** A
    // transition records a barrier *and* advances the tracking, but a discarded
    // recording never executes, so the image is still resting where it was
    // while the tracking says otherwise - and the next barrier would name an
    // oldLayout the image is not in. Only the owned-recording path can undo it:
    // a failure part-way through an *open* recording has already put barriers
    // on a command buffer that is going to run.
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

    // One rectangle of one level of one face. The bytes are repacked tight into
    // the upload arena rather than described to Vulkan with a bufferRowLength,
    // because the caller's stride is in *bytes* and Vulkan's is in texels - a
    // capture buffer's 4096-byte row of a 1000-texel image has no texel count
    // to name it with. The repack is one memcpy a row of a copy that was about
    // to cross the bus anyway.
    //
    // **A row here is whatever the format says a row is**, which for a
    // block-compressed one is a row of 4x4 blocks: levelBytesPerRow and
    // levelRows answer in blocks, so this loop needs no block arithmetic of its
    // own, while imageExtent stays in texels, which is what Vulkan wants either
    // way.
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

    // The whole texture: one face or six, as one level or as a chain. A cube's
    // faces follow one another in the source block, each of them sourcePitch
    // times the level's rows on from the last - the layout
    // TextureDescriptor::cube describes, and the same one the other two
    // backends walk.
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

    // One face - which on a 2D texture is the whole texture - as one level, as
    // a chain built from that face's own pixels, or as the chain the caller
    // supplied. The three differ only in where each level's bytes come from.
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

    // Every level of one face, out of a chain eacp built or one the caller
    // handed over. The supplied one is walked rather than produced - each level
    // starts levelBytes past the last, which is the layout MipChain packs - and
    // no filter of eacp's own runs anywhere near it, which is the whole of what
    // TextureDescriptor::mipLevels is for.
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

    // The whole texture, whether that is one level or a chain on each of six
    // faces. Every subresource is written while the image sits in TRANSFER_DST
    // and it goes back to resting once, however many there were.
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

        // Tightly packed by definition, so a stride is a number that can only
        // be wrong - dropped rather than used as a pitch it cannot be, in the
        // same words the other two backends use. See Texture::update.
        if (bytesPerRow != 0 && (suppliedChain || isCompressedFormat(format)))
            return;

        // A cube goes the way a mipmapped texture does, and for the same
        // reason: more than one subresource to fill. A compressed texture joins
        // them whatever its level count, the other path being the region one
        // and a region of blocks being refused there.
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

        // Six rectangles this could mean and nothing here to say which -
        // dropped rather than sent to +X. See the header.
        if (data.cube)
            return;

        // A compressed rectangle would have to land on the 4x4 block grid, so
        // the rect a caller wrote and the rect the GPU updated would differ by
        // up to three texels a side. Dropped rather than rounded.
        if (isCompressedFormat(format))
            return;

        // Out of bounds is dropped rather than clamped - see the header for why
        // clamping would silently upload skewed pixels.
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

    // ---------------------------------------------------------------- reading

    // Both read() overloads land here, as the update() pair land in
    // updateRegion. A recording of its own rather than the open one, because
    // the pixels are wanted now and an open recording cannot be waited on -
    // which is the rule the header states: a read is valid once the work that
    // drew the texture has been committed, and Frame::flush() is what makes
    // that true inside a frame.
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

        // Six faces and no argument to name one, exactly as the region upload
        // has none; and blocks rather than pixels, with no decoder at either
        // end. Both dropped - see the header.
        if (data.cube || isCompressedFormat(format))
            return;

        // Dropped rather than clamped, exactly as the upload side drops it.
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

        // Back to where a texture rests between passes, so the next bind finds
        // a sampleable image whatever this one was doing before the read.
        transitionTextureForUse(commands->buffer, data, data.restingUse());

        // The copy went onto the same queue as the writes, so waiting for this
        // submission also waits for them.
        context.waitFor(context.submit(commands));

        // Coherent memory, so what the GPU wrote is what the CPU reads with no
        // invalidate in between - see VulkanContext::makeHostBuffer.
        const auto stride =
            bytesPerRow != 0 ? static_cast<std::size_t>(bytesPerRow) : rowBytes;
        auto* out = static_cast<std::byte*>(dst);

        for (auto row = 0; row < regionHeight; ++row)
            std::memcpy(out + static_cast<std::size_t>(row) * stride,
                        mapped + static_cast<std::size_t>(row) * rowBytes,
                        rowBytes);
    }

    // The Device's context, held for the texture's lifetime: the images were
    // allocated against its allocator, every upload runs on its queue, and the
    // deferred release belongs to its timeline. A texture never moves between
    // Devices.
    VulkanContext& context;

    // Every size in this file comes out of the format through levelBytesPerRow
    // and levelBytes rather than out of a stored stride, a compressed format
    // having no per-texel size to store.
    TextureFormat format = TextureFormat::RGBA8Unorm;

    // Whether the levels arrived with the pixels rather than being built from
    // level 0, which changes where each level's bytes come from and nothing
    // else. See TextureDescriptor::mipLevels.
    bool suppliedChain = false;

    // Asked for *and* granted, which is not the same thing: the format and the
    // device both have a say. It is what turns the storage view on.
    bool computeWrite = false;

    // Mutable because the layout tracking advances inside the const read(): the
    // copy into the readback buffer is a use like any other, and Buffer's
    // Native carries the same note for the same reason.
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
    // Texels are whole; round rather than truncate so a rect built from
    // accumulated float arithmetic lands on the texel it is nearest to.
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
    // Rounded rather than truncated, as update()'s region is.
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

// The depth images, the multisample companion and the resolved depth all live
// inside the VulkanTextureData nativeTexture hands back, which is what a pass
// reaches them through - so this backend has no separate handles to give, for
// the same reason D3D12 has none.
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
