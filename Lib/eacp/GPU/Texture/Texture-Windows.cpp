#include <eacp/Core/Utils/WinInclude.h>

#include "Texture.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"
#include "MipChain.h"
#include "TextureShape.h"

#include <cmath>

// Windows/D3D12 backend. A texture is a default-heap resource plus an SRV
// descriptor living in the context's shader-visible heap for the texture's
// whole lifetime, so binding is a single root-table update. Pixels
// upload through a transient row-pitch-aligned staging buffer; the resource
// then stays in PIXEL_SHADER_RESOURCE state forever (it is only ever sampled).

namespace eacp::GPU
{
namespace
{
// Exhaustive, in the same words the Metal backend uses: a format added without
// a DXGI counterpart is a -Wswitch warning rather than a resource quietly
// created as RGBA8, which is what the default this replaced would have made of
// every one of the block formats.
DXGI_FORMAT toDXGIFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::BGRA8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::R8Unorm:
            return DXGI_FORMAT_R8_UNORM;
        case TextureFormat::RG8Unorm:
            return DXGI_FORMAT_R8G8_UNORM;
        case TextureFormat::RGBA16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::RGBA32Float:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case TextureFormat::R32Float:
            return DXGI_FORMAT_R32_FLOAT;
        case TextureFormat::R16Float:
            return DXGI_FORMAT_R16_FLOAT;
        case TextureFormat::RG16Float:
            return DXGI_FORMAT_R16G16_FLOAT;
        case TextureFormat::RG32Float:
            return DXGI_FORMAT_R32G32_FLOAT;
        case TextureFormat::R16Unorm:
            return DXGI_FORMAT_R16_UNORM;
        case TextureFormat::R32Uint:
            return DXGI_FORMAT_R32_UINT;
        case TextureFormat::RGBA8UnormSrgb:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case TextureFormat::BGRA8UnormSrgb:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case TextureFormat::RG16Unorm:
            return DXGI_FORMAT_R16G16_UNORM;
        case TextureFormat::RGBA16Unorm:
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        case TextureFormat::RGB10A2Unorm:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case TextureFormat::RG11B10Float:
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case TextureFormat::RGB9E5Float:
            return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        case TextureFormat::R8Uint:
            return DXGI_FORMAT_R8_UINT;
        case TextureFormat::RG8Uint:
            return DXGI_FORMAT_R8G8_UINT;
        case TextureFormat::RGBA8Uint:
            return DXGI_FORMAT_R8G8B8A8_UINT;
        case TextureFormat::R16Uint:
            return DXGI_FORMAT_R16_UINT;
        case TextureFormat::RG16Uint:
            return DXGI_FORMAT_R16G16_UINT;
        case TextureFormat::RGBA16Uint:
            return DXGI_FORMAT_R16G16B16A16_UINT;
        case TextureFormat::RG32Uint:
            return DXGI_FORMAT_R32G32_UINT;
        case TextureFormat::RGBA32Uint:
            return DXGI_FORMAT_R32G32B32A32_UINT;
        case TextureFormat::R8Sint:
            return DXGI_FORMAT_R8_SINT;
        case TextureFormat::RG8Sint:
            return DXGI_FORMAT_R8G8_SINT;
        case TextureFormat::RGBA8Sint:
            return DXGI_FORMAT_R8G8B8A8_SINT;
        case TextureFormat::R16Sint:
            return DXGI_FORMAT_R16_SINT;
        case TextureFormat::RG16Sint:
            return DXGI_FORMAT_R16G16_SINT;
        case TextureFormat::RGBA16Sint:
            return DXGI_FORMAT_R16G16B16A16_SINT;
        case TextureFormat::R32Sint:
            return DXGI_FORMAT_R32_SINT;
        case TextureFormat::RG32Sint:
            return DXGI_FORMAT_R32G32_SINT;
        case TextureFormat::RGBA32Sint:
            return DXGI_FORMAT_R32G32B32A32_SINT;
        case TextureFormat::BC1RGBA:
            return DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC2RGBA:
            return DXGI_FORMAT_BC2_UNORM;
        case TextureFormat::BC3RGBA:
            return DXGI_FORMAT_BC3_UNORM;
        case TextureFormat::BC7RGBA:
            return DXGI_FORMAT_BC7_UNORM;
        case TextureFormat::BC1RGBASrgb:
            return DXGI_FORMAT_BC1_UNORM_SRGB;
        case TextureFormat::BC2RGBASrgb:
            return DXGI_FORMAT_BC2_UNORM_SRGB;
        case TextureFormat::BC3RGBASrgb:
            return DXGI_FORMAT_BC3_UNORM_SRGB;
        case TextureFormat::BC7RGBASrgb:
            return DXGI_FORMAT_BC7_UNORM_SRGB;
        case TextureFormat::BC4RUnorm:
            return DXGI_FORMAT_BC4_UNORM;
        case TextureFormat::BC4RSnorm:
            return DXGI_FORMAT_BC4_SNORM;
        case TextureFormat::BC5RGUnorm:
            return DXGI_FORMAT_BC5_UNORM;
        case TextureFormat::BC5RGSnorm:
            return DXGI_FORMAT_BC5_SNORM;
        case TextureFormat::BC6HRGBUfloat:
            return DXGI_FORMAT_BC6H_UF16;
    }

    return DXGI_FORMAT_UNKNOWN;
}

} // namespace

DXGI_FORMAT d3d12TextureFormat(TextureFormat format)
{
    return toDXGIFormat(format);
}

namespace
{
// The filter/address translations that used to live here are gone with the
// sampler descriptors they filled in; the equivalent mapping is now in
// D3D12Context::createRootSignatures, which builds the static samplers.

bool isLevelZeroOfSliceZero(int level, int slice, int count)
{
    return level == 0 && slice == 0 && count == 1;
}

D3D12_CPU_DESCRIPTOR_HANDLE makeTargetView(
    D3D12TextureData& texture, bool depth, int level, int slice, int count)
{
    for (const auto& view: texture.targetViews)
        if (view.depth == depth && view.level == level && view.slice == slice
            && view.count == count)
            return view.handle;

    auto* device = getD3D12Shared().getDevice();
    auto* resource = depth ? texture.depthResource.get() : texture.resource.get();

    if (device == nullptr || resource == nullptr)
        return {};

    auto view = D3D12SubresourceTargetView {level, slice, count, depth};

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type =
        depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;

    if (FAILED(device->CreateDescriptorHeap(
            &heapDesc, __uuidof(ID3D12DescriptorHeap), view.heap.put_void())))
        return {};

    view.handle = view.heap->GetCPUDescriptorHandleForHeapStart();

    const auto first = static_cast<UINT>(slice);
    const auto size = static_cast<UINT>(count);
    const auto mip = static_cast<UINT>(level);

    if (depth)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
        viewDesc.Format = depthAttachmentFormat(texture.depthHasStencil);
        viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.MipSlice = mip;
        viewDesc.Texture2DArray.FirstArraySlice = first;
        viewDesc.Texture2DArray.ArraySize = size;
        device->CreateDepthStencilView(resource, &viewDesc, view.handle);
    }
    else
    {
        D3D12_RENDER_TARGET_VIEW_DESC viewDesc = {};
        viewDesc.Format = texture.colorFormat;

        if (resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        {
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            viewDesc.Texture3D.MipSlice = mip;
            viewDesc.Texture3D.FirstWSlice = first;
            viewDesc.Texture3D.WSize = size;
        }
        else
        {
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.MipSlice = mip;
            viewDesc.Texture2DArray.FirstArraySlice = first;
            viewDesc.Texture2DArray.ArraySize = size;
        }

        device->CreateRenderTargetView(resource, &viewDesc, view.handle);
    }

    const auto handle = view.handle;
    texture.targetViews.add(std::move(view));
    return handle;
}
} // namespace

D3D12_CPU_DESCRIPTOR_HANDLE
d3d12ColorTargetView(D3D12TextureData& texture, int level, int slice, int count)
{
    if (isLevelZeroOfSliceZero(level, slice, count) || texture.isMultisampled())
        return texture.attachmentView();

    return makeTargetView(texture, false, level, slice, count);
}

D3D12_CPU_DESCRIPTOR_HANDLE
d3d12DepthTargetView(D3D12TextureData& texture, int level, int slice, int count)
{
    if (!texture.hasDepth())
        return {};

    if (isLevelZeroOfSliceZero(level, slice, count) || texture.isMultisampled())
        return texture.dsv;

    return makeTargetView(texture, true, level, slice, count);
}

struct Texture::Native
{
    Native(Device& device,
           const TextureDescriptor& descriptor,
           const void* pixels,
           InitialData initialData)
        : context(getD3D12Context(device))
        , width(descriptor.width)
        , height(descriptor.height)
        , format(descriptor.format)
    {
        if (!context.isValid())
            return;

        const auto resolved = resolveTextureShape(descriptor, initialData);

        if (!resolved)
            return;

        shape = *resolved;
        levels = shape.levels;

        // Refused before anything is created, in the same words the Metal
        // backend uses. See TextureDescriptor::sampleCount.
        if (descriptor.renderTarget && descriptor.sampleCount > 1)
        {
            if (!device.supportsSampleCount(descriptor.sampleCount))
                return;

            data.sampleCount = descriptor.sampleCount;
        }

        // See the block formats and RGB9E5Float in Texture.h: sampled only.
        if (!isRenderableFormat(format)
            && (descriptor.renderTarget || descriptor.computeWrite))
            return;

        if (isCompressedFormat(format) && !device.supportsBlockCompression())
            return;

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = shape.isVolume() ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                          : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);

        // A cube is a six-element 2D array here, and it is the *view* that makes
        // it a cube - D3D12 has no cube resource dimension, only
        // D3D12_SRV_DIMENSION_TEXTURECUBE over six array slices, which
        // createDescriptors builds below. The slice order is the array order,
        // which is the same +X, -X, +Y, -Y, +Z, -Z the Metal backend uploads in;
        // a cube array is layers of six.
        desc.DepthOrArraySize =
            static_cast<UINT16>(shape.isVolume() ? shape.depth : shape.slices());
        desc.MipLevels = static_cast<UINT16>(levels);
        desc.Format = toDXGIFormat(descriptor.format);
        desc.SampleDesc.Count = 1;

        if (descriptor.renderTarget)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        // Refused before the resource exists, so a kernel output the device
        // cannot actually store to is an invalid texture rather than a dispatch
        // that writes nothing.
        if (descriptor.computeWrite)
        {
            if (!device.supportsComputeWrite(descriptor.format))
                return;

            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            computeWrite = true;
        }

        // Frame::generateMipmaps reads and writes the levels through UAVs, so
        // a texture whose chain the GPU may fill is created able to have them;
        // a texture of one level, or in a format the kernel cannot load and
        // store, is not.
        if (levels > 1 && data.sampleCount <= 1
            && device.supportsMipGeneration(descriptor.format))
        {
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            generatesMips = true;
        }

        auto initialState = pixels != nullptr
                                ? D3D12_RESOURCE_STATE_COPY_DEST
                                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        data.state = initialState;

        if (FAILED(context.getDevice()->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                initialState,
                nullptr,
                __uuidof(ID3D12Resource),
                data.resource.put_void())))
            return;

        if (pixels != nullptr)
            upload(context, pixels);

        if (data.resource != nullptr)
            createDescriptors(context, descriptor);
    }

    // D3D12CalcSubresource, spelled out: the levels of one array slice are
    // consecutive, and the slices follow one another. A volume has one slice.
    UINT subresourceIndex(int level, int slice) const
    {
        return static_cast<UINT>(level + slice * levels);
    }

    // Zero-copy wrapping of a shared D3D11/DXGI surface into the D3D12 backend
    // is a planned optimisation; until then the camera/video path uploads via
    // update(). A null resource yields an invalid texture, which the higher
    // layer detects and falls back from.
    Native(Device& device, void*)
        : context(getD3D12Context(device))
    {
    }

    ~Native()
    {
        context.freeTextureDescriptor(data.srv);
        context.freeTextureDescriptor(data.uav);
        context.freeTextureDescriptor(data.depthSrv);
        context.freeTextureDescriptor(data.msaaDepthSrv);

        for (auto& slot: data.levelSrvs)
            context.freeTextureDescriptor(slot);

        for (auto& slot: data.levelUavs)
            context.freeTextureDescriptor(slot);

        for (auto& slot: data.arraySrvs)
            context.freeTextureDescriptor(slot);

        for (auto& slot: data.depthSliceSrvs)
            context.freeTextureDescriptor(slot);

        for (auto& view: data.targetViews)
            context.deferRelease(std::move(view.heap));
        context.deferRelease(std::move(data.rtvHeap));
        context.deferRelease(std::move(data.msaaRtvHeap));
        context.deferRelease(std::move(data.dsvHeap));
        context.deferRelease(std::move(data.resolvedDsvHeap));
        context.deferRelease(std::move(data.depthResource));
        context.deferRelease(std::move(data.resolvedDepthResource));
        context.deferRelease(std::move(data.msaaResource));
        context.deferRelease(std::move(data.resource));
    }

    // Maps a staging buffer, copies the source's rows (and a volume's images)
    // into the 256-byte-aligned rows GetCopyableFootprints reports, and records
    // the copy. The resource must already be in COPY_DEST. Returns false on a
    // staging failure, having recorded nothing.
    //
    // **Rows here are whatever GetCopyableFootprints says a row is**, which for
    // a block-compressed format is a row of 4x4 blocks, so the source pitch is
    // measured the same way: levelBytesPerRow, which is what the plan carries.
    bool copyPixels(D3D12Context& context,
                    CommandContext* commands,
                    const SubresourceUpload& upload)
    {
        auto regionDesc = data.resource->GetDesc();

        // Footprints at the *region's* size, one level and one slice: this
        // describes the staging side, a flat box of pixels.
        regionDesc.Width = static_cast<UINT64>(upload.width);
        regionDesc.Height = static_cast<UINT>(upload.height);
        regionDesc.MipLevels = 1;
        regionDesc.DepthOrArraySize =
            static_cast<UINT16>(shape.isVolume() ? upload.depth : 1);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        context.getDevice()->GetCopyableFootprints(
            &regionDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);

        auto* staging = context.acquireStagingBuffer(*commands, totalBytes);

        if (staging == nullptr)
            return false;

        void* mapped = nullptr;
        const D3D12_RANGE noRead = {0, 0};

        if (FAILED(staging->Map(0, &noRead, &mapped)))
            return false;

        const auto copyBytes = static_cast<std::size_t>(rowBytes);
        const auto rowPitch = static_cast<std::size_t>(footprint.Footprint.RowPitch);
        const auto* source = static_cast<const unsigned char*>(upload.pixels);

        for (auto image = 0; image < upload.depth; ++image)
            for (auto row = UINT {0}; row < rows; ++row)
                std::memcpy(
                    static_cast<unsigned char*>(mapped)
                        + (static_cast<std::size_t>(image) * rows + row) * rowPitch,
                    source
                        + static_cast<std::size_t>(image)
                              * static_cast<std::size_t>(upload.bytesPerImage)
                        + static_cast<std::size_t>(row)
                              * static_cast<std::size_t>(upload.bytesPerRow),
                    copyBytes);

        staging->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = data.resource.get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = subresourceIndex(upload.level, upload.slice);

        D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
        sourceLocation.pResource = staging;
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;

        commands->list->CopyTextureRegion(&destination,
                                          static_cast<UINT>(upload.x),
                                          static_cast<UINT>(upload.y),
                                          static_cast<UINT>(upload.z),
                                          &sourceLocation,
                                          nullptr);

        // Not parked in transients: the staging pool owns the buffer and takes
        // it back once this recording's fence passes.
        return true;
    }

    // At creation: the resource was created in COPY_DEST, so the copies record
    // with no leading barrier.
    void upload(D3D12Context& context, const void* pixels)
    {
        const auto plan = planWholeUpload(shape, format, pixels, 0);

        if (!plan.valid)
        {
            data.resource = nullptr;
            return;
        }

        auto* commands = context.acquire();

        if (commands == nullptr)
        {
            data.resource = nullptr;
            return;
        }

        if (!recordUploads(context, commands, plan.uploads))
        {
            context.discard(commands);
            data.resource = nullptr;
            return;
        }

        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context.submit(commands);
    }

    bool recordUploads(D3D12Context& context,
                       CommandContext* commands,
                       const Vector<SubresourceUpload>& uploads)
    {
        for (const auto& each: uploads)
            if (!copyPixels(context, commands, each))
                return false;

        return true;
    }

    // Every later upload: the resource moves to COPY_DEST from wherever it is
    // resting, every subresource is copied, and it moves back once.
    //
    // **The tracked state goes back where it was if nothing is submitted.**
    // transitionTextureForUse records a barrier *and* advances data.state, but a
    // discarded list never executes, so the next transition would otherwise
    // name a StateBefore the resource is not in.
    void submitUploads(const Vector<SubresourceUpload>& uploads)
    {
        if (data.resource == nullptr || uploads.empty() || !context.isValid())
            return;

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        const auto stateBefore = data.state;

        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_COPY_DEST);

        if (!recordUploads(context, commands, uploads))
        {
            data.state = stateBefore;
            context.discard(commands);
            return;
        }

        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context.submit(commands);
    }

    void update(const void* pixels, int bytesPerRow)
    {
        const auto plan = planWholeUpload(shape, format, pixels, bytesPerRow);

        if (plan.valid)
            submitUploads(plan.uploads);
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

    // Every read lands here. The download side of copyPixels, and the same
    // three pieces: the footprint of the *region*, a pooled staging resource in
    // the read-back heap, and the row pitch it reports, which is 256-byte
    // aligned and almost never the row the caller wants.
    //
    // The submission is waited on rather than left running - the pixels are
    // wanted now, which is what a read-back is - and the resource is put back
    // where the rest of this file expects to find it.
    void read(const TextureRegion& region, void* dst) const
    {
        if (data.resource == nullptr || !context.isValid())
            return;

        const auto plan = planRegionRead(shape, format, region, dst);

        if (!plan)
            return;

        // A driver that will not copy a box out of a texture
        // (DriverQuirks::noBoxedTextureCopy) is asked for the whole subresource
        // and the region is cut out of it below.
        const auto wholeCopy = getD3D12Shared().getDriverQuirks().noBoxedTextureCopy;
        const auto copyWidth =
            wholeCopy ? shape.levelWidth(plan->level) : plan->width;
        const auto copyHeight =
            wholeCopy ? shape.levelHeight(plan->level) : plan->height;
        const auto copyDepth =
            wholeCopy ? shape.levelDepth(plan->level) : plan->depth;

        auto copyDesc = data.resource->GetDesc();
        copyDesc.Width = static_cast<UINT64>(copyWidth);
        copyDesc.Height = static_cast<UINT>(copyHeight);
        copyDesc.MipLevels = 1;
        copyDesc.DepthOrArraySize =
            static_cast<UINT16>(shape.isVolume() ? copyDepth : 1);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        context.getDevice()->GetCopyableFootprints(
            &copyDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        auto* staging = context.acquireReadbackBuffer(*commands, totalBytes);

        if (staging == nullptr)
        {
            context.discard(commands);
            return;
        }

        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = staging;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = data.resource.get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = subresourceIndex(plan->level, plan->slice);

        D3D12_BOX box = {};
        box.left = static_cast<UINT>(plan->x);
        box.top = static_cast<UINT>(plan->y);
        box.front = static_cast<UINT>(plan->z);
        box.right = static_cast<UINT>(plan->x + plan->width);
        box.bottom = static_cast<UINT>(plan->y + plan->height);
        box.back = static_cast<UINT>(plan->z + plan->depth);

        commands->list->CopyTextureRegion(
            &destination, 0, 0, 0, &source, wholeCopy ? nullptr : &box);

        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // The copy went onto the same queue as the writes, so waiting for this
        // submission's fence also waits for them.
        context.waitFor(context.submit(commands));

        void* mapped = nullptr;
        const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};

        if (FAILED(staging->Map(0, &readRange, &mapped)) || mapped == nullptr)
            return;

        const auto texelBytes =
            static_cast<std::size_t>(rowBytes) / static_cast<std::size_t>(copyWidth);
        const auto regionRowBytes =
            texelBytes * static_cast<std::size_t>(plan->width);
        const auto rowPitch = static_cast<std::size_t>(footprint.Footprint.RowPitch);
        const auto imagePitch = rowPitch * rows;

        const auto skipImages = wholeCopy ? static_cast<std::size_t>(plan->z) : 0;
        const auto skipRows = wholeCopy ? static_cast<std::size_t>(plan->y) : 0;
        const auto skipBytes =
            wholeCopy ? texelBytes * static_cast<std::size_t>(plan->x) : 0;

        auto* out = static_cast<unsigned char*>(dst);
        const auto* in = static_cast<const unsigned char*>(mapped) + skipBytes;

        for (auto image = std::size_t {0};
             image < static_cast<std::size_t>(plan->depth);
             ++image)
            for (auto row = std::size_t {0};
                 row < static_cast<std::size_t>(plan->height);
                 ++row)
                std::memcpy(
                    out + image * static_cast<std::size_t>(plan->bytesPerImage)
                        + row * static_cast<std::size_t>(plan->bytesPerRow),
                    in + (image + skipImages) * imagePitch
                        + (row + skipRows) * rowPitch,
                    regionRowBytes);

        const D3D12_RANGE noWrite = {0, 0};
        staging->Unmap(0, &noWrite);
    }

    // A render target's own one-descriptor RTV heap. RTV descriptors are not
    // shader-visible, so there is no shared heap to take one from the way the
    // SRV does - and a heap per target is what keeps a texture's descriptor
    // alive for exactly as long as the texture is.
    void createRenderTargetView(D3D12Context& context,
                                const TextureDescriptor& descriptor)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;

        if (FAILED(context.getDevice()->CreateDescriptorHeap(
                &heapDesc, __uuidof(ID3D12DescriptorHeap), data.rtvHeap.put_void())))
            return;

        D3D12_RENDER_TARGET_VIEW_DESC viewDesc = {};
        viewDesc.Format = toDXGIFormat(descriptor.format);
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        // Level 0 of slice 0, which is what a pass without a level or slice
        // renders into; an array resource needs an array view to name it.
        if (shape.isVolume())
        {
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            viewDesc.Texture3D.WSize = 1;
        }
        else if (shape.kind != TextureType::TwoD)
        {
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.ArraySize = 1;
        }

        auto handle = data.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        context.getDevice()->CreateRenderTargetView(
            data.resource.get(), &viewDesc, handle);

        data.rtv = handle;
        data.colorFormat = viewDesc.Format;
    }

    // The multisample resource a pass into this target actually renders into,
    // and its own one-descriptor RTV heap - the same shape the single-sampled
    // RTV above has, and for the same reason.
    //
    // It rests in RENDER_TARGET, as the drawable's MSAA target does, and the
    // resolve at the end of each pass steps it out to RESOLVE_SOURCE and back.
    // The optimised clear value has to be there and has to agree with what
    // beginPass clears to, which is nothing in particular - the pass names its
    // own colour - so this takes black and accepts the mismatch warning a clear
    // to another colour costs, exactly as the drawable's target does.
    void createMultisampleTarget(D3D12Context& context,
                                 const TextureDescriptor& descriptor)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = toDXGIFormat(descriptor.format);
        desc.SampleDesc.Count = static_cast<UINT>(data.sampleCount);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = desc.Format;

        if (FAILED(context.getDevice()->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                &clearValue,
                __uuidof(ID3D12Resource),
                data.msaaResource.put_void())))
            return;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;

        if (FAILED(context.getDevice()->CreateDescriptorHeap(
                &heapDesc,
                __uuidof(ID3D12DescriptorHeap),
                data.msaaRtvHeap.put_void())))
        {
            data.msaaResource = nullptr;
            return;
        }

        D3D12_RENDER_TARGET_VIEW_DESC viewDesc = {};
        viewDesc.Format = desc.Format;
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;

        auto handle = data.msaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
        context.getDevice()->CreateRenderTargetView(
            data.msaaResource.get(), &viewDesc, handle);

        data.msaaRtv = handle;
        data.msaaState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // The depth buffer a pass into this target attaches, and its DSV. Created
    // in DEPTH_WRITE and left there: nothing else ever uses the resource, so it
    // needs no barrier and no state tracking - unless `sampleable` says a shader
    // is going to read it too, which is what createDepthShaderResourceView adds
    // and the one case where the state moves.
    //
    // The optimised clear value is not optional. D3D12 wants a resource that
    // will be cleared to say so at creation, and a ClearDepthStencilView that
    // does not match it is a validation error rather than a slow path - and it
    // must agree with the 1.0 the pass clears to. It names the *attachment*
    // format even where the resource is typeless, which is the format the clear
    // is actually performed in.
    void createDepthBuffer(D3D12Context& context, bool withStencil, bool sampleable)
    {
        const auto format = depthAttachmentFormat(withStencil);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        // The colour target's levels and slices, so a pass into any level or
        // layer of it finds a depth plane of the same size there.
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = static_cast<UINT16>(shape.slices());
        desc.MipLevels = static_cast<UINT16>(levels);
        // At the colour attachment's count, because both APIs require every
        // attachment of one pass to agree - a multisampled target with a
        // single-sampled depth buffer is not a pass either of them will run.
        //
        // The *resource* is only created typeless where the buffer itself is
        // going to be read; on a multisampled target the thing a shader reads is
        // the resolve below, and this one keeps the fully typed attachment
        // format it would have had without sampleableDepth at all - unless the
        // resolve is going to be drawn, in which case the shader drawing it
        // reads this one too. See resolveDepthWithShader.
        const auto multisampled = data.sampleCount > 1;
        const auto readByShader =
            !multisampled || getD3D12Shared().getDriverQuirks().noDepthResolve;

        desc.Format = depthResourceFormat(withStencil, sampleable && readByShader);
        desc.SampleDesc.Count = static_cast<UINT>(data.sampleCount);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        // DENY_SHADER_RESOURCE is not set either way, so this flag says nothing
        // by its absence; what makes the buffer readable is the typeless format
        // above and the SRV below.
        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = format;
        clearValue.DepthStencil.Depth = 1.f;

        if (FAILED(context.getDevice()->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &clearValue,
                __uuidof(ID3D12Resource),
                data.depthResource.put_void())))
            return;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = 1;

        if (FAILED(context.getDevice()->CreateDescriptorHeap(
                &heapDesc, __uuidof(ID3D12DescriptorHeap), data.dsvHeap.put_void())))
        {
            data.depthResource = nullptr;
            return;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
        viewDesc.Format = format;
        viewDesc.ViewDimension = multisampled ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                              : D3D12_DSV_DIMENSION_TEXTURE2D;

        if (shape.kind != TextureType::TwoD)
        {
            viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.ArraySize = 1;
        }

        auto handle = data.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        context.getDevice()->CreateDepthStencilView(
            data.depthResource.get(), &viewDesc, handle);

        data.dsv = handle;
        data.depthHasStencil = withStencil;
        data.depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        data.msaaDepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        if (!sampleable)
            return;

        // On a multisampled target the buffer a shader reads is not the
        // attachment: a Texture2DMS is not what ShaderBuilder::depthTexture
        // declares. So the plane is resolved into a single-sampled twin at the
        // end of every pass and the SRV views that instead. See
        // TextureDescriptor::sampleCount.
        if (multisampled && !createResolvedDepthBuffer(context, withStencil))
            return;

        if (multisampled && getD3D12Shared().getDriverQuirks().noDepthResolve
            && !createShaderResolveViews(context, withStencil))
            return;

        createDepthShaderResourceView(context, withStencil);
    }

    // What resolveDepthWithShader draws with, where the driver leaves it to:
    // the multisampled attachment as a Texture2DMS for the pixel shader to
    // load, and the resolved buffer as the depth attachment it writes. A
    // failure leaves the target without a sampleable depth, exactly as a
    // failed resolve buffer does.
    bool createShaderResolveViews(D3D12Context& context, bool withStencil)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = 1;

        if (FAILED(context.getDevice()->CreateDescriptorHeap(
                &heapDesc,
                __uuidof(ID3D12DescriptorHeap),
                data.resolvedDsvHeap.put_void())))
            return false;

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = depthAttachmentFormat(withStencil);
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

        data.resolvedDsv =
            data.resolvedDsvHeap->GetCPUDescriptorHandleForHeapStart();
        context.getDevice()->CreateDepthStencilView(
            data.resolvedDepthResource.get(), &dsvDesc, data.resolvedDsv);

        data.msaaDepthSrv = context.allocateTextureDescriptor();

        if (data.msaaDepthSrv.cpu.ptr == 0)
            return false;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = depthShaderResourceFormat(withStencil);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        context.getDevice()->CreateShaderResourceView(
            data.depthResource.get(), &srvDesc, data.msaaDepthSrv.cpu);
        return true;
    }

    // The single-sampled destination of that resolve, created typeless so the
    // SRV can name one float channel out of it, and resting in DEPTH_WRITE for
    // the same reason the attachment does - it is what transitionDepthForUse
    // moves, and starting both in one state keeps the tracking honest.
    //
    // ALLOW_DEPTH_STENCIL rather than nothing: a resolve destination for a
    // depth format has to be a depth resource, ResolveSubresourceRegion refusing
    // one that is not.
    bool createResolvedDepthBuffer(D3D12Context& context, bool withStencil)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = depthResourceFormat(withStencil, true);
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = depthAttachmentFormat(withStencil);
        clearValue.DepthStencil.Depth = 1.f;

        return SUCCEEDED(context.getDevice()->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            __uuidof(ID3D12Resource),
            data.resolvedDepthResource.put_void()));
    }

    // The read view: one float channel, out of the same shader-visible heap the
    // colour SRV comes from. A failure here leaves the target with a depth
    // buffer it can still attach and not sample, which is what
    // hasSampleableDepth answers and what a bind through it checks.
    //
    // The resource it views is the attachment on a single-sampled target and the
    // resolve on a multisampled one, which is the whole of what sampledDepth-
    // Resource is for.
    void createDepthShaderResourceView(D3D12Context& context, bool withStencil)
    {
        data.depthSrv = context.allocateTextureDescriptor();

        if (data.depthSrv.cpu.ptr == 0)
            return;

        D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
        viewDesc.Format = depthShaderResourceFormat(withStencil);
        viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        viewDesc.Texture2D.MipLevels = 1;

        // An array target's depth reads as the array it is, level 0 of every
        // slice.
        if (shape.kind != TextureType::TwoD)
        {
            viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.MipLevels = 1;
            viewDesc.Texture2DArray.ArraySize = static_cast<UINT>(shape.slices());
        }

        context.getDevice()->CreateShaderResourceView(
            data.sampledDepthResource(), &viewDesc, data.depthSrv.cpu);

        if (shape.kind != TextureType::TwoD)
            createDepthSliceViews(context, viewDesc.Format);
    }

    void createDepthSliceViews(D3D12Context& context, DXGI_FORMAT viewFormat)
    {
        for (auto slice = 0; slice < shape.slices(); ++slice)
        {
            auto read = context.allocateTextureDescriptor();

            if (read.cpu.ptr != 0)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
                viewDesc.Format = viewFormat;
                viewDesc.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                viewDesc.Texture2DArray.MipLevels = 1;
                viewDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(slice);
                viewDesc.Texture2DArray.ArraySize = 1;

                context.getDevice()->CreateShaderResourceView(
                    data.sampledDepthResource(), &viewDesc, read.cpu);
            }

            data.depthSliceSrvs.add(read);
        }
    }

    // The view a kernel writes through. It comes out of the same shader-visible
    // heap as the SRV - the allocator is CBV_SRV_UAV - so a writable texture
    // costs one more descriptor and no new heap.
    void createUnorderedAccessView(D3D12Context& context,
                                   const TextureDescriptor& descriptor)
    {
        data.uav = context.allocateTextureDescriptor();

        if (data.uav.cpu.ptr == 0)
        {
            computeWrite = false;
            return;
        }

        const auto viewDesc =
            describeUnorderedAccess(toDXGIFormat(descriptor.format), 0);

        context.getDevice()->CreateUnorderedAccessView(
            data.resource.get(), nullptr, &viewDesc, data.uav.cpu);
    }

    // One level, every slice: a cube or an array as a RWTexture2DArray, a
    // volume as a RWTexture3D of all its images.
    D3D12_UNORDERED_ACCESS_VIEW_DESC describeUnorderedAccess(DXGI_FORMAT viewFormat,
                                                             int level) const
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};
        viewDesc.Format = viewFormat;

        if (shape.isVolume())
        {
            viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            viewDesc.Texture3D.MipSlice = static_cast<UINT>(level);
            viewDesc.Texture3D.WSize = static_cast<UINT>(shape.levelDepth(level));
        }
        else if (shape.kind != TextureType::TwoD)
        {
            viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.MipSlice = static_cast<UINT>(level);
            viewDesc.Texture2DArray.ArraySize = static_cast<UINT>(shape.slices());
        }
        else
        {
            viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipSlice = static_cast<UINT>(level);
        }

        return viewDesc;
    }

    // Every level from `level` down, every slice, with the dimension the
    // shader declares for the kind.
    D3D12_SHADER_RESOURCE_VIEW_DESC describeShaderResource(DXGI_FORMAT viewFormat,
                                                           int level,
                                                           int levelCount) const
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
        viewDesc.Format = viewFormat;
        viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        const auto mostDetailed = static_cast<UINT>(level);
        const auto count = static_cast<UINT>(levelCount);

        switch (shape.kind)
        {
            case TextureType::TwoD:
                viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                viewDesc.Texture2D.MostDetailedMip = mostDetailed;
                viewDesc.Texture2D.MipLevels = count;
                break;

            case TextureType::Cube:
                viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                viewDesc.TextureCube.MostDetailedMip = mostDetailed;
                viewDesc.TextureCube.MipLevels = count;
                break;

            case TextureType::TwoDArray:
                viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                viewDesc.Texture2DArray.MostDetailedMip = mostDetailed;
                viewDesc.Texture2DArray.MipLevels = count;
                viewDesc.Texture2DArray.ArraySize = static_cast<UINT>(shape.layers);
                break;

            case TextureType::CubeArray:
                viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                viewDesc.TextureCubeArray.MostDetailedMip = mostDetailed;
                viewDesc.TextureCubeArray.MipLevels = count;
                viewDesc.TextureCubeArray.NumCubes = static_cast<UINT>(shape.layers);
                break;

            case TextureType::ThreeD:
                viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                viewDesc.Texture3D.MostDetailedMip = mostDetailed;
                viewDesc.Texture3D.MipLevels = count;
                break;
        }

        return viewDesc;
    }

    // A read view and, on a computeWrite texture, a write view of each level
    // alone, for ComputePass's level binds. A slot that fails to allocate
    // leaves that level unbindable rather than the texture invalid. A texture
    // without them binds level 0 only, through its whole views.
    void createLevelViews(D3D12Context& context, DXGI_FORMAT viewFormat)
    {
        for (auto level = 0; level < levels; ++level)
        {
            auto read = context.allocateTextureDescriptor();

            if (read.cpu.ptr != 0)
            {
                const auto readDesc = describeShaderResource(viewFormat, level, 1);
                context.getDevice()->CreateShaderResourceView(
                    data.resource.get(), &readDesc, read.cpu);
            }

            data.levelSrvs.add(read);

            if (!computeWrite && !generatesMips)
                continue;

            auto write = context.allocateTextureDescriptor();

            if (write.cpu.ptr != 0)
            {
                const auto writeDesc = describeUnorderedAccess(viewFormat, level);
                context.getDevice()->CreateUnorderedAccessView(
                    data.resource.get(), nullptr, &writeDesc, write.cpu);
            }

            data.levelUavs.add(write);
        }
    }

    // A cube or a cube array read as the 2D array of slices it is stored as,
    // one view per level, for ComputePass::setInputTextureAsArray.
    void createArrayViews(D3D12Context& context, DXGI_FORMAT viewFormat)
    {
        for (auto level = 0; level < levels; ++level)
        {
            auto read = context.allocateTextureDescriptor();

            if (read.cpu.ptr != 0)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
                viewDesc.Format = viewFormat;
                viewDesc.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                viewDesc.Texture2DArray.MostDetailedMip = static_cast<UINT>(level);
                viewDesc.Texture2DArray.MipLevels = 1;
                viewDesc.Texture2DArray.ArraySize =
                    static_cast<UINT>(shape.slices());

                context.getDevice()->CreateShaderResourceView(
                    data.resource.get(), &viewDesc, read.cpu);
            }

            data.arraySrvs.add(read);
        }
    }

    void createDescriptors(D3D12Context& context,
                           const TextureDescriptor& descriptor)
    {
        if (descriptor.renderTarget)
            createRenderTargetView(context, descriptor);

        // Created after the RTV and refused with it: a multisampled target is
        // either both resources or nothing, since a pass rendering into the
        // single-sampled one would silently be a different pass.
        if (data.sampleCount > 1 && data.rtv.ptr != 0)
        {
            createMultisampleTarget(context, descriptor);

            if (!data.isMultisampled())
            {
                data.resource = nullptr;
                return;
            }
        }

        if (descriptor.renderTarget
            && (descriptor.depth || descriptor.stencil || descriptor.sampleableDepth)
            && data.rtv.ptr != 0)
            createDepthBuffer(
                context, descriptor.stencil, descriptor.sampleableDepth);

        if (computeWrite)
            createUnorderedAccessView(context, descriptor);

        data.srv = context.allocateTextureDescriptor();

        if (data.srv.cpu.ptr == 0)
            return;

        // **The view is what makes a cube, an array or a volume what it is on
        // this backend.** A cube's resource is a six-slice 2D array and nothing
        // about it says otherwise, so a TextureCube declaration needs the
        // TEXTURECUBE view; an array declared Texture2DArray needs the array
        // view even with one layer. Every level, as the null description gave.
        const auto viewDesc =
            describeShaderResource(toDXGIFormat(descriptor.format), 0, levels);

        context.getDevice()->CreateShaderResourceView(
            data.resource.get(), &viewDesc, data.srv.cpu);

        // Only where the GPU fills the levels, so a mipmapped texture that is
        // only ever sampled does not spend a descriptor per level.
        if (levels > 1 && (computeWrite || descriptor.renderTarget || generatesMips))
            createLevelViews(context, toDXGIFormat(descriptor.format));

        if (shape.kind == TextureType::Cube || shape.kind == TextureType::CubeArray)
            createArrayViews(context, toDXGIFormat(descriptor.format));

        // No sampler descriptor is created, and TextureDescriptor::filter and
        // ::addressMode are unused on this backend: the sampler comes from the
        // root signature's static samplers, picked by the shader's declared
        // TextureSampling. A sampler descriptor table cannot be relied on here -
        // see D3D12Context::createRootSignatures. Allocating one anyway would
        // also let the 256-entry sampler heap fail texture creation for nothing.
    }

    // The Device's context, held for the texture's lifetime: the descriptor
    // slots came out of its heaps and go back to them, and every upload runs
    // on its queue. A texture never moves between Devices.
    D3D12Context& context;

    int width = 0;
    int height = 0;

    // Every size in this file comes out of the format through levelBytesPerRow
    // and levelBytes rather than out of a stored stride, because a compressed
    // format has no per-texel size to store. The (stubbed) zero-copy wrap path
    // leaves this at RGBA8Unorm, which is the four bytes those buffers always
    // carry.
    TextureFormat format = TextureFormat::RGBA8Unorm;

    TextureShape shape;

    // The resource's MipLevels, and the stride between one slice's
    // subresources and the next.
    int levels = 1;

    bool computeWrite = false;

    // Created with per-level UAVs for Frame::generateMipmaps' kernel.
    bool generatesMips = false;

    // Mutable because the state tracking advances inside the const read(): the
    // copy to the read-back buffer is a use like any other, and Buffer's
    // Native carries the same note for the same reason.
    mutable D3D12TextureData data;
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
        impl->context.deferRelease(std::move(impl->data.resource));
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

    // Texels are whole; round rather than truncate so a rect built from
    // accumulated float arithmetic lands on the texel it is nearest to.
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

    impl->readRegion(0, 0, impl->width, impl->height, dst, bytesPerRow);
}

void Texture::read(const Graphics::Rect& region, void* dst, int bytesPerRow) const
{
    if (bytesPerRow < 0)
        return;

    // Rounded rather than truncated, as update()'s region is.
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
    return impl->width;
}

int Texture::height() const
{
    return impl->height;
}

TextureFormat Texture::format() const
{
    return impl->format;
}

bool Texture::isValid() const
{
    return impl->data.resource != nullptr && impl->data.srv.cpu.ptr != 0;
}

int Texture::mipLevels() const
{
    return impl->levels;
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
    return const_cast<D3D12TextureData*>(&impl->data);
}

void* Texture::nativeReadView() const
{
    return const_cast<D3D12TextureData*>(&impl->data);
}

// The depth resource and its descriptor live inside the same D3D12TextureData
// nativeTexture hands back, which is what Frame reaches them through - so this
// backend has no separate handle to give.
void* Texture::nativeDepthTexture() const
{
    return nullptr;
}

// Both of these are Metal's, for the same reason the depth handle above is
// null here: the multisample colour resource and the resolved depth buffer live
// inside the D3D12TextureData nativeTexture hands back, and every Windows
// translation unit that wants one reaches it through that.
void* Texture::nativeMultisampleTexture() const
{
    return nullptr;
}

void* Texture::nativeResolvedDepthTexture() const
{
    return nullptr;
}
} // namespace eacp::GPU
