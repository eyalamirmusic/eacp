#include <eacp/Core/Utils/WinInclude.h>

#include "Texture.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"
#include "MipChain.h"

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

        // No sRGB variants here for the reason Texture.h gives: eacp has no
        // sRGB formats at all, so DXT1 with or without punch-through alpha is
        // this one BC1_UNORM either way.
        case TextureFormat::BC1RGBA:
            return DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC2RGBA:
            return DXGI_FORMAT_BC2_UNORM;
        case TextureFormat::BC3RGBA:
            return DXGI_FORMAT_BC3_UNORM;
        case TextureFormat::BC7RGBA:
            return DXGI_FORMAT_BC7_UNORM;
    }

    return DXGI_FORMAT_UNKNOWN;
}

// The filter/address translations that used to live here are gone with the
// sampler descriptors they filled in; the equivalent mapping is now in
// D3D12Context::createRootSignatures, which builds the static samplers.

// Whether this device can take a typed UAV store to the format. The formats
// supportsComputeWrite() allows are the ones the specification guarantees, but
// a guarantee is not a driver, so it is asked rather than assumed - a kernel
// whose stores are silently dropped is a great deal harder to find than a
// texture that refused to be created.
bool supportsTypedUAVStore(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
    support.Format = format;

    if (FAILED(device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        return false;

    return (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}
} // namespace

struct Texture::Native
{
    Native(Device& device, const TextureDescriptor& descriptor, const void* pixels)
        : context(getD3D12Context(device))
        , width(descriptor.width)
        , height(descriptor.height)
        , format(descriptor.format)
        , cube(descriptor.cube)
    {
        if (!context.isValid() || width <= 0 || height <= 0)
            return;

        // A cube's faces are square and there are six of them, so a rectangle -
        // or a cube something on the GPU would write - has nothing this could
        // create. Refused in the same words the Metal backend uses, so the two
        // agree on what is not a texture. See TextureDescriptor::cube.
        if (cube
            && (width != height || descriptor.renderTarget
                || descriptor.computeWrite))
            return;

        // Refused before anything is created, in the same words the Metal
        // backend uses: a target the device cannot multisample is an invalid
        // texture rather than one drawing at a count the pipelines compiled for
        // it do not carry. See TextureDescriptor::sampleCount.
        if (descriptor.renderTarget && descriptor.sampleCount > 1)
        {
            if (cube || !device.supportsSampleCount(descriptor.sampleCount))
                return;

            data.sampleCount = descriptor.sampleCount;
        }

        // A compressed texture is a block of bytes the sampler decodes and
        // nothing else, so there is no per-texel address for a pass or a kernel
        // to write to. Refused in the same words the Metal backend uses, and the
        // device is asked for the format because a caller has a choice to make
        // when the answer is no - Device::supportsBlockCompression says yes on
        // every Direct3D device, and that is a fact about D3D rather than about
        // this call. See the block formats in Texture.h.
        if (isCompressedFormat(format))
        {
            if (descriptor.renderTarget || descriptor.computeWrite)
                return;

            if (!device.supportsBlockCompression())
                return;
        }

        if (descriptor.mipLevels < 0)
            return;

        // A caller-supplied chain is taken as it is, and everything that would
        // make it a chain nobody could have supplied is refused rather than
        // reconciled - see TextureDescriptor::mipLevels, and the Metal backend,
        // which refuses the same six things.
        if (descriptor.mipLevels > 0)
        {
            if (descriptor.mipmapped || descriptor.renderTarget
                || descriptor.computeWrite || cube)
                return;

            // Nothing was handed over, so there is no chain to take - refused
            // rather than created empty, in the same words the Metal backend
            // uses.
            if (pixels == nullptr)
                return;

            if (descriptor.mipLevels > mipLevelCount(width, height))
                return;

            levels = descriptor.mipLevels;
            suppliedChain = true;
        }
        // A chain eacp builds needs pixels to build it from and a format it can
        // average: a render target or a kernel output has none at creation, and
        // a compressed one has no average, so either would get levels nothing
        // ever writes and the sampler would read them.
        else if (descriptor.mipmapped && pixels != nullptr
                 && canBuildMipChain(format))
        {
            levels = mipLevelCount(width, height);
        }

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);

        // A cube is a six-element 2D array here, and it is the *view* that makes
        // it a cube - D3D12 has no cube resource dimension, only
        // D3D12_SRV_DIMENSION_TEXTURECUBE over six array slices, which
        // createDescriptors builds below. The slice order is the array order,
        // which is the same +X, -X, +Y, -Y, +Z, -Z the Metal backend uploads in.
        desc.DepthOrArraySize = static_cast<UINT16>(cube ? 6 : 1);
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
            if (!supportsComputeWrite(descriptor.format)
                || !supportsTypedUAVStore(context.getDevice(), desc.Format))
                return;

            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            computeWrite = true;
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
        context.deferRelease(std::move(data.rtvHeap));
        context.deferRelease(std::move(data.msaaRtvHeap));
        context.deferRelease(std::move(data.dsvHeap));
        context.deferRelease(std::move(data.depthResource));
        context.deferRelease(std::move(data.resolvedDepthResource));
        context.deferRelease(std::move(data.msaaResource));
        context.deferRelease(std::move(data.resource));
    }

    // Maps a staging buffer, copies each source row's pixels (advancing the
    // source by sourcePitch to skip any padding) into the
    // 256-byte-aligned staging rows GetCopyableFootprints reports, then records
    // the copy and the transition back to PIXEL_SHADER_RESOURCE. The resource
    // must already be in COPY_DEST. Returns false on a staging failure, having
    // recorded nothing.
    //
    // **Rows here are whatever GetCopyableFootprints says a row is**, which for
    // a block-compressed format is a row of 4x4 blocks: NumRows comes back as
    // the level's height in blocks and RowSizeInBytes as its width in blocks
    // times the block's size. So this loop needs no block arithmetic of its own -
    // only a sourcePitch measured the same way, which is what levelBytesPerRow
    // gives its callers. Every compressed copy covers a whole subresource at
    // (0, 0), which is what keeps it inside D3D12's rule that a BC copy be
    // block-aligned or cover the level.
    bool copyPixels(D3D12Context& context,
                    CommandContext* commands,
                    const void* pixels,
                    std::size_t sourcePitch,
                    int destX,
                    int destY,
                    int regionWidth,
                    int regionHeight,
                    int mipLevel = 0,
                    bool transitionAfterwards = true,
                    int face = 0)
    {
        auto desc = data.resource->GetDesc();

        // Footprints are asked for at the *region's* size, not the texture's, so
        // the staging buffer and its row pitch describe only what is being
        // uploaded. The copy is then placed at destX/destY in the destination.
        auto regionDesc = desc;
        regionDesc.Width = static_cast<UINT64>(regionWidth);
        regionDesc.Height = static_cast<UINT>(regionHeight);

        // One level and one slice, always: this describes the *staging* side,
        // which is a flat rectangle of pixels. Leaving the resource's own counts
        // here would ask for the footprint of a chain a region this size cannot
        // have, and - on a cube - of six faces where one is being uploaded.
        regionDesc.MipLevels = 1;
        regionDesc.DepthOrArraySize = 1;

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

        auto copyBytes = static_cast<std::size_t>(rowBytes);

        for (auto row = UINT {0}; row < rows; ++row)
            std::memcpy(static_cast<unsigned char*>(mapped)
                            + row * footprint.Footprint.RowPitch,
                        static_cast<const unsigned char*>(pixels)
                            + row * sourcePitch,
                        copyBytes);

        staging->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = data.resource.get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        // D3D12CalcSubresource, spelled out: the mip levels of one array slice
        // are consecutive, and the slices follow one another. On a 2D texture
        // the face is 0 and this is the level, which is what it was before cube
        // textures existed.
        destination.SubresourceIndex = static_cast<UINT>(mipLevel + face * levels);

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = staging;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;

        commands->list->CopyTextureRegion(&destination,
                                          static_cast<UINT>(destX),
                                          static_cast<UINT>(destY),
                                          0,
                                          &source,
                                          nullptr);

        // Held back while a chain is mid-upload: every level goes into the same
        // resource, so transitioning after each one would move it out of
        // COPY_DEST and straight back for the next - a pair of barriers per
        // level, for nothing.
        if (transitionAfterwards)
            transitionTextureForUse(commands->list.get(),
                                    data,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Not parked in transients: the staging pool owns the buffer and takes
        // it back once this recording's fence passes.
        return true;
    }

    void upload(D3D12Context& context, const void* pixels)
    {
        auto* commands = context.acquire();

        if (commands == nullptr)
        {
            data.resource = nullptr;
            return;
        }

        // The resource was created in COPY_DEST, so the copies record with no
        // leading barrier.
        if (!recordLevels(
                context, commands, pixels, levelBytesPerRow(format, width)))
        {
            context.discard(commands);
            data.resource = nullptr;
            return;
        }

        context.submit(commands);
    }

    // The whole texture: one face or six, as one level or as a chain. Every
    // subresource is copied before the resource moves back to
    // PIXEL_SHADER_RESOURCE, so the transition happens once however many there
    // are.
    //
    // A cube's faces follow one another in the source block, each of them
    // sourcePitch * height bytes on from the last - the layout
    // TextureDescriptor::cube describes, and the same one the Metal backend
    // walks.
    bool recordLevels(D3D12Context& context,
                      CommandContext* commands,
                      const void* pixels,
                      std::size_t sourcePitch)
    {
        const auto faces = cube ? 6 : 1;
        const auto faceBytes =
            sourcePitch * static_cast<std::size_t>(levelRows(format, height));

        for (auto face = 0; face < faces; ++face)
        {
            const auto* facePixels = static_cast<const unsigned char*>(pixels)
                                     + static_cast<std::size_t>(face) * faceBytes;

            // The very last subresource of the very last face is what puts the
            // resource back where a bind expects it, and nothing before it does.
            const auto isLast = face == faces - 1;

            if (!recordFace(
                    context, commands, facePixels, sourcePitch, face, isLast))
                return false;
        }

        return true;
    }

    // One face - which on a 2D texture is the whole texture - as one level, as a
    // chain built from that face's own pixels, or as the chain the caller
    // supplied. The three differ only in where each level's bytes come from.
    bool recordFace(D3D12Context& context,
                    CommandContext* commands,
                    const void* pixels,
                    std::size_t sourcePitch,
                    int face,
                    bool transitionAfterwards)
    {
        if (levels <= 1)
            return copyPixels(context,
                              commands,
                              pixels,
                              sourcePitch,
                              0,
                              0,
                              width,
                              height,
                              0,
                              transitionAfterwards,
                              face);

        if (suppliedChain)
            return recordSuppliedChain(
                context, commands, pixels, face, transitionAfterwards);

        const auto chain = buildMipChain(pixels, width, height, format, sourcePitch);

        if (!chain.isValid())
            return false;

        for (auto level = 0; level < levels; ++level)
        {
            const auto levelWidth = mipExtent(width, level);
            const auto levelHeight = mipExtent(height, level);

            if (!copyPixels(context,
                            commands,
                            chain.level(level),
                            levelBytesPerRow(format, levelWidth),
                            0,
                            0,
                            levelWidth,
                            levelHeight,
                            level,
                            transitionAfterwards && level == levels - 1,
                            face))
                return false;
        }

        return true;
    }

    // The same loop with the source walked rather than produced: the levels are
    // already in the block MipChain packs, so each one starts levelBytes past
    // the last. No filter runs anywhere here, which is the whole of what
    // TextureDescriptor::mipLevels is for.
    bool recordSuppliedChain(D3D12Context& context,
                             CommandContext* commands,
                             const void* pixels,
                             int face,
                             bool transitionAfterwards)
    {
        const auto* bytes = static_cast<const unsigned char*>(pixels);

        for (auto level = 0; level < levels; ++level)
        {
            const auto levelWidth = mipExtent(width, level);
            const auto levelHeight = mipExtent(height, level);

            if (!copyPixels(context,
                            commands,
                            bytes,
                            levelBytesPerRow(format, levelWidth),
                            0,
                            0,
                            levelWidth,
                            levelHeight,
                            level,
                            transitionAfterwards && level == levels - 1,
                            face))
                return false;

            bytes += levelBytes(format, levelWidth, levelHeight);
        }

        return true;
    }

    void update(const void* pixels, std::size_t bytesPerRow)
    {
        // Tightly packed by definition, so a stride is a number that can only be
        // wrong - dropped rather than used as a pitch it cannot be, in the same
        // words the Metal backend uses. See Texture::update.
        if (bytesPerRow != 0 && (suppliedChain || isCompressedFormat(format)))
            return;

        // A cube goes the same way a mipmapped texture does, and for the same
        // reason: more than one subresource to fill, so the resource has to be
        // moved back to COPY_DEST once and every face written before it returns.
        // A compressed texture joins them whatever its level count, because the
        // other path is the region one and a region of blocks is refused there.
        if (levels > 1 || cube || isCompressedFormat(format))
        {
            updateWholeTexture(pixels, bytesPerRow);
            return;
        }

        updateRegion(0, 0, width, height, pixels, bytesPerRow);
    }

    // The re-upload path for everything the region one cannot take - a mip
    // chain, six cube faces, blocks, or any combination. Unlike updateRegion it
    // has to move the resource back to COPY_DEST first, since by now it is being
    // sampled.
    //
    // **The tracked state goes back where it was if nothing is submitted.**
    // transitionTextureForUse records a barrier *and* advances data.state, but a
    // discarded list is closed without ever executing - so the resource is still
    // resting where it was while the tracking says COPY_DEST, and the next
    // transition would name a StateBefore the resource is not in: a debug-layer
    // error, and undefined behaviour without one. The shape predates this change
    // - updateRegion below avoids it by submitting either way - but every
    // compressed and every supplied-chain update now comes through here, so it
    // is worth closing rather than leaving on a path only staging failure
    // reaches.
    void updateWholeTexture(const void* pixels, std::size_t bytesPerRow)
    {
        if (data.resource == nullptr || pixels == nullptr)
            return;

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        const auto stateBefore = data.state;

        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_COPY_DEST);

        const auto pitch =
            bytesPerRow > 0 ? bytesPerRow : levelBytesPerRow(format, width);

        if (!recordLevels(context, commands, pixels, pitch))
        {
            data.state = stateBefore;
            context.discard(commands);
            return;
        }

        context.submit(commands);
    }

    // Both update() overloads land here; the whole-texture one is just the full
    // rect, so there is a single upload path to reason about.
    void updateRegion(int x,
                      int y,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      std::size_t bytesPerRow)
    {
        if (data.resource == nullptr || pixels == nullptr || width <= 0
            || height <= 0)
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        // A cube has six rectangles this could mean, and nothing here says
        // which - see the header. Dropped rather than sent to +X.
        if (cube)
            return;

        // A compressed rectangle would have to land on the 4x4 block grid, so
        // the rect asked for and the rect written would differ by up to three
        // texels a side. Dropped rather than rounded, in the same words the
        // Metal backend uses.
        if (isCompressedFormat(format))
            return;

        // Out-of-bounds is dropped rather than clamped — see the header for why
        // clamping would silently upload skewed pixels.
        if (x < 0 || y < 0 || x + regionWidth > width || y + regionHeight > height)
            return;

        if (!context.isValid())
            return;

        auto* commands = context.acquire();

        if (commands == nullptr)
            return;

        auto sourcePitch =
            bytesPerRow != 0 ? bytesPerRow : levelBytesPerRow(format, regionWidth);

        // The resource rests in PIXEL_SHADER_RESOURCE between frames - or in
        // RENDER_TARGET, if it is one a pass last drew into - so the move to
        // COPY_DEST goes through the tracked state rather than assuming which.
        // Staging failing puts it back, so the next bind still sees a sampleable
        // resource.
        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_COPY_DEST);

        if (!copyPixels(context,
                        commands,
                        pixels,
                        sourcePitch,
                        x,
                        y,
                        regionWidth,
                        regionHeight))
            transitionTextureForUse(commands->list.get(),
                                    data,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        context.submit(commands);
    }

    // Both read() overloads land here, as the update() pair land in
    // updateRegion. The download side of copyPixels, and the same three pieces:
    // the footprint of the *region*, a pooled staging resource in the read-back
    // heap, and the row pitch it reports, which is 256-byte aligned and almost
    // never the row the caller wants.
    //
    // The submission is waited on rather than left running - the pixels are
    // wanted now, which is what a read-back is - and the resource is put back
    // where the rest of this file expects to find it.
    void readRegion(int x,
                    int y,
                    int regionWidth,
                    int regionHeight,
                    void* dst,
                    std::size_t bytesPerRow) const
    {
        if (data.resource == nullptr || dst == nullptr || !context.isValid())
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        // Six faces and no argument to name one, exactly as the region upload
        // has none - see the header.
        if (cube)
            return;

        // Blocks rather than pixels, and no decoder at either end - see the
        // header for why a compressed read-back has nothing to hand back.
        if (isCompressedFormat(format))
            return;

        // Dropped rather than clamped, exactly as the upload side drops it.
        if (x < 0 || y < 0 || x + regionWidth > width || y + regionHeight > height)
            return;

        auto regionDesc = data.resource->GetDesc();
        regionDesc.Width = static_cast<UINT64>(regionWidth);
        regionDesc.Height = static_cast<UINT>(regionHeight);
        regionDesc.MipLevels = 1;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        context.getDevice()->GetCopyableFootprints(
            &regionDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);

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
        source.SubresourceIndex = 0;

        D3D12_BOX box = {};
        box.left = static_cast<UINT>(x);
        box.top = static_cast<UINT>(y);
        box.front = 0;
        box.right = static_cast<UINT>(x + regionWidth);
        box.bottom = static_cast<UINT>(y + regionHeight);
        box.back = 1;

        commands->list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);

        // Back to where a texture rests between frames, so the next bind finds
        // a sampleable resource whatever this one was doing before the read.
        transitionTextureForUse(
            commands->list.get(), data, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // The copy went onto the same queue as the writes, so waiting for this
        // submission's fence also waits for them - which is the rule the header
        // states, and the same one Buffer::read leans on.
        context.waitFor(context.submit(commands));

        void* mapped = nullptr;
        const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};

        if (FAILED(staging->Map(0, &readRange, &mapped)) || mapped == nullptr)
            return;

        const auto stride =
            bytesPerRow != 0 ? bytesPerRow : static_cast<std::size_t>(rowBytes);

        auto* out = static_cast<unsigned char*>(dst);
        const auto* in = static_cast<const unsigned char*>(mapped);

        for (auto row = UINT {0}; row < rows; ++row)
            std::memcpy(
                out + static_cast<std::size_t>(row) * stride,
                in + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                static_cast<std::size_t>(rowBytes));

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

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        // At the colour attachment's count, because both APIs require every
        // attachment of one pass to agree - a multisampled target with a
        // single-sampled depth buffer is not a pass either of them will run.
        //
        // The *resource* is only created typeless where the buffer itself is
        // going to be read; on a multisampled target the thing a shader reads is
        // the resolve below, and this one keeps the fully typed attachment
        // format it would have had without sampleableDepth at all.
        const auto multisampled = data.sampleCount > 1;

        desc.Format = depthResourceFormat(withStencil, sampleable && !multisampled);
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

        createDepthShaderResourceView(context, withStencil);
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

        context.getDevice()->CreateShaderResourceView(
            data.sampledDepthResource(), &viewDesc, data.depthSrv.cpu);
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

        D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};
        viewDesc.Format = toDXGIFormat(descriptor.format);
        viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        context.getDevice()->CreateUnorderedAccessView(
            data.resource.get(), nullptr, &viewDesc, data.uav.cpu);
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

        // **The view is what makes a cube a cube on this backend.** The resource
        // underneath is a six-slice 2D array and nothing about it says
        // otherwise, so a null description - which is what every 2D texture here
        // takes, and what covers every level a resource has - would hand the
        // shader a Texture2DArray. A TextureCube declaration reading one is a
        // dimension mismatch: the debug layer says so, and a release runtime
        // samples nothing at all.
        //
        // MipLevels is spelled out because the description is no longer the
        // null one: -1 is D3D12's "all of them from MostDetailedMip down", and
        // is what the null description was giving before.
        if (cube)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
            viewDesc.Format = toDXGIFormat(descriptor.format);
            viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            viewDesc.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            viewDesc.TextureCube.MostDetailedMip = 0;
            viewDesc.TextureCube.MipLevels = static_cast<UINT>(-1);
            viewDesc.TextureCube.ResourceMinLODClamp = 0.f;

            context.getDevice()->CreateShaderResourceView(
                data.resource.get(), &viewDesc, data.srv.cpu);
        }
        else
        {
            context.getDevice()->CreateShaderResourceView(
                data.resource.get(), nullptr, data.srv.cpu);
        }

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

    // 1 unless a chain was asked for and there were pixels to build one from, or
    // the caller supplied one of its own. It is the resource's MipLevels, the
    // stride between one face's subresources and the next in copyPixels, and the
    // bound of every upload loop; the SRV alone does not need it, being built
    // from a null description that covers whatever levels the resource has -
    // except on a cube, where createDescriptors spells the TEXTURECUBE view out.
    int levels = 1;

    // Whether those levels arrived with the pixels rather than being built from
    // level 0, which changes where each level's bytes come from and nothing
    // else. See TextureDescriptor::mipLevels.
    bool suppliedChain = false;

    bool computeWrite = false;

    // Six array slices under a cube view rather than one 2D image, which changes
    // the resource's array size, the subresource each upload lands on, the SRV,
    // and what the two region-shaped entry points do - and nothing else.
    bool cube = false;

    // Mutable because the state tracking advances inside the const read(): the
    // copy to the read-back buffer is a use like any other, and Buffer's
    // Native carries the same note for the same reason.
    mutable D3D12TextureData data;
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

void Texture::update(const void* pixels, std::size_t bytesPerRow)
{
    impl->update(pixels, bytesPerRow);
}

void Texture::update(const Graphics::Rect& region,
                     const void* pixels,
                     std::size_t bytesPerRow)
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

void Texture::read(void* dst, std::size_t bytesPerRow) const
{
    impl->readRegion(0, 0, impl->width, impl->height, dst, bytesPerRow);
}

void Texture::read(const Graphics::Rect& region,
                   void* dst,
                   std::size_t bytesPerRow) const
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
    return impl->width;
}

int Texture::height() const
{
    return impl->height;
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
    return isValid() && impl->cube;
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
