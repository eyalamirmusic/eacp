#include <eacp/Core/Utils/WinInclude.h>

#include "CopyRecorder.h"

#include "../Windows/D3D12Types.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>

namespace eacp::GPU
{
namespace
{
D3D12TextureData* textureDataOf(const Texture& texture)
{
    auto* data = static_cast<D3D12TextureData*>(texture.nativeTexture());
    return data != nullptr && data->resource != nullptr ? data : nullptr;
}

D3D12BufferData* bufferDataOf(const Buffer& buffer)
{
    auto* data = static_cast<D3D12BufferData*>(buffer.nativeBuffer());
    return data != nullptr && data->resource != nullptr ? data : nullptr;
}

UINT subresourceOf(const D3D12TextureData& data, int level, int slice)
{
    const auto levels = data.resource->GetDesc().MipLevels;
    return static_cast<UINT>(level) + static_cast<UINT>(slice) * levels;
}

D3D12_TEXTURE_COPY_LOCATION
    subresourceLocation(const D3D12TextureData& data, int level, int slice)
{
    D3D12_TEXTURE_COPY_LOCATION location = {};
    location.pResource = data.resource.get();
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location.SubresourceIndex = subresourceOf(data, level, slice);
    return location;
}

D3D12_TEXTURE_COPY_LOCATION footprintLocation(ID3D12Resource* buffer,
                                              const D3D12TextureData& texture,
                                              UINT64 offset,
                                              const BufferTextureCopyPlan& plan,
                                              UINT rowPitch)
{
    D3D12_TEXTURE_COPY_LOCATION location = {};
    location.pResource = buffer;
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint.Offset = offset;
    location.PlacedFootprint.Footprint.Format = texture.resource->GetDesc().Format;
    location.PlacedFootprint.Footprint.Width = static_cast<UINT>(plan.width);
    location.PlacedFootprint.Footprint.Height = static_cast<UINT>(plan.height);
    location.PlacedFootprint.Footprint.Depth = 1;
    location.PlacedFootprint.Footprint.RowPitch = rowPitch;
    return location;
}

D3D12_BOX boxOf(int x, int y, int width, int height, int z = 0, int depth = 1)
{
    D3D12_BOX box = {};
    box.left = static_cast<UINT>(x);
    box.top = static_cast<UINT>(y);
    box.front = static_cast<UINT>(z);
    box.right = static_cast<UINT>(x + width);
    box.bottom = static_cast<UINT>(y + height);
    box.back = static_cast<UINT>(z + depth);
    return box;
}

int depthOf(const Texture& texture, int level)
{
    return texture.kind() == TextureType::ThreeD
               ? std::max(1, texture.volumeDepth() >> level)
               : 1;
}

bool isWholeLevel(const Texture& texture, int level, int x, int y, int w, int h)
{
    return x == 0 && y == 0 && w == levelWidth(texture, level)
           && h == levelHeight(texture, level);
}

// A plain texture rests in PIXEL_SHADER_RESOURCE and every later use moves it
// from wherever the tracking says, so a copy hands it back there.
void restAfterCopy(ID3D12GraphicsCommandList* list, D3D12TextureData& data)
{
    transitionTextureForUse(list, data, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// CopyTextureRegion wants a footprint's rows on the 256-byte grid and its
// offset on the 512-byte one; eacp asks only for the texel grid. Where the two
// disagree the rows go through a scratch buffer laid out the way D3D12 wants.
bool isFootprintAligned(const BufferTextureCopyPlan& plan)
{
    return plan.bytesPerRow % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0
           && plan.bufferOffset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0;
}

UINT alignedPitch(const BufferTextureCopyPlan& plan)
{
    constexpr auto alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    const auto rowBytes = static_cast<UINT>(plan.rowBytes());
    return (rowBytes + alignment - 1) / alignment * alignment;
}

ID3D12Resource* makeScratch(CommandContext& commands, UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto scratch = winrt::com_ptr<ID3D12Resource> {};

    if (FAILED(getD3D12Shared().getDevice()->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            __uuidof(ID3D12Resource),
            scratch.put_void())))
        return nullptr;

    auto* raw = scratch.get();
    commands.transients.add(std::move(scratch));
    return raw;
}

void copyRows(ID3D12GraphicsCommandList* list,
              ID3D12Resource* destination,
              UINT64 destinationOffset,
              UINT64 destinationPitch,
              ID3D12Resource* source,
              UINT64 sourceOffset,
              UINT64 sourcePitch,
              const BufferTextureCopyPlan& plan)
{
    for (auto row = 0; row < plan.height; ++row)
        list->CopyBufferRegion(destination,
                               destinationOffset + destinationPitch * row,
                               source,
                               sourceOffset + sourcePitch * row,
                               static_cast<UINT64>(plan.rowBytes()));
}
} // namespace

void recordCopy(void* recording, const TextureCopyPlan& plan)
{
    auto* commands = static_cast<CommandContext*>(recording);
    auto* source = textureDataOf(*plan.source);
    auto* destination = textureDataOf(*plan.destination);

    if (commands == nullptr || source == nullptr || destination == nullptr)
        return;

    const auto whole = isWholeLevel(*plan.source,
                                    plan.sourceLevel,
                                    plan.sourceX,
                                    plan.sourceY,
                                    plan.width,
                                    plan.height)
                       && plan.destinationX == 0 && plan.destinationY == 0
                       && plan.sourceZ == 0 && plan.destinationZ == 0
                       && plan.depth == depthOf(*plan.source, plan.sourceLevel);

    if (!whole && getD3D12Shared().getDriverQuirks().noBoxedTextureCopy)
    {
        LOG("D3D12: this driver copies whole subresources only, so a region copy "
            "is dropped");
        return;
    }

    auto* list = commands->list.get();

    transitionTextureForUse(list, *source, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionTextureForUse(list, *destination, D3D12_RESOURCE_STATE_COPY_DEST);

    const auto box = boxOf(plan.sourceX,
                           plan.sourceY,
                           plan.width,
                           plan.height,
                           plan.sourceZ,
                           plan.depth);

    // One subresource per slice; a volume's images are the box's depth.
    for (auto slice = 0; slice < plan.sliceCount; ++slice)
    {
        const auto from =
            subresourceLocation(*source, plan.sourceLevel, plan.sourceSlice + slice);
        const auto to = subresourceLocation(
            *destination, plan.destinationLevel, plan.destinationSlice + slice);

        list->CopyTextureRegion(&to,
                                static_cast<UINT>(plan.destinationX),
                                static_cast<UINT>(plan.destinationY),
                                static_cast<UINT>(plan.destinationZ),
                                &from,
                                whole ? nullptr : &box);
    }

    restAfterCopy(list, *source);
    restAfterCopy(list, *destination);
}

void recordCopy(void* recording, const BufferCopyPlan& plan)
{
    auto* commands = static_cast<CommandContext*>(recording);
    auto* source = bufferDataOf(*plan.source);
    auto* destination = bufferDataOf(*plan.destination);

    if (commands == nullptr || source == nullptr || destination == nullptr)
        return;

    if (destination->uploadHeap)
    {
        LOG("D3D12: a streaming buffer cannot be copied into");
        return;
    }

    transitionForUse(*commands, *source, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionForUse(*commands, *destination, D3D12_RESOURCE_STATE_COPY_DEST);

    commands->list->CopyBufferRegion(destination->resource.get(),
                                     static_cast<UINT64>(plan.destinationOffset),
                                     source->resource.get(),
                                     static_cast<UINT64>(plan.sourceOffset),
                                     static_cast<UINT64>(plan.bytes));
}

void recordCopyToTexture(void* recording, const BufferTextureCopyPlan& plan)
{
    auto* commands = static_cast<CommandContext*>(recording);
    auto* buffer = bufferDataOf(*plan.buffer);
    auto* texture = textureDataOf(*plan.texture);

    if (commands == nullptr || buffer == nullptr || texture == nullptr)
        return;

    auto* list = commands->list.get();
    auto* source = buffer->resource.get();
    auto offset = static_cast<UINT64>(plan.bufferOffset);
    auto pitch = static_cast<UINT>(plan.bytesPerRow);

    transitionForUse(*commands, *buffer, D3D12_RESOURCE_STATE_COPY_SOURCE);

    if (!isFootprintAligned(plan))
    {
        pitch = alignedPitch(plan);
        auto* scratch =
            makeScratch(*commands, static_cast<UINT64>(pitch) * plan.height);

        if (scratch == nullptr)
            return;

        copyRows(list,
                 scratch,
                 0,
                 pitch,
                 source,
                 offset,
                 static_cast<UINT64>(plan.bytesPerRow),
                 plan);
        transition(list,
                   scratch,
                   D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);

        source = scratch;
        offset = 0;
    }

    transitionTextureForUse(list, *texture, D3D12_RESOURCE_STATE_COPY_DEST);

    const auto from = footprintLocation(source, *texture, offset, plan, pitch);
    const auto to = subresourceLocation(*texture, plan.level, plan.slice);

    list->CopyTextureRegion(&to,
                            static_cast<UINT>(plan.x),
                            static_cast<UINT>(plan.y),
                            0,
                            &from,
                            nullptr);

    restAfterCopy(list, *texture);
}

void recordCopyToBuffer(void* recording, const BufferTextureCopyPlan& plan)
{
    auto* commands = static_cast<CommandContext*>(recording);
    auto* buffer = bufferDataOf(*plan.buffer);
    auto* texture = textureDataOf(*plan.texture);

    if (commands == nullptr || buffer == nullptr || texture == nullptr)
        return;

    if (buffer->uploadHeap)
    {
        LOG("D3D12: a streaming buffer cannot be copied into");
        return;
    }

    if (getD3D12Shared().getDriverQuirks().noBoxedTextureCopy
        && !isWholeLevel(
            *plan.texture, plan.level, plan.x, plan.y, plan.width, plan.height))
    {
        LOG("D3D12: this driver copies whole subresources only, so a region copy "
            "is dropped");
        return;
    }

    auto* list = commands->list.get();
    const auto aligned = isFootprintAligned(plan);
    const auto pitch =
        aligned ? static_cast<UINT>(plan.bytesPerRow) : alignedPitch(plan);

    auto* destination = buffer->resource.get();
    auto offset = static_cast<UINT64>(plan.bufferOffset);

    if (aligned)
    {
        transitionForUse(*commands, *buffer, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    else
    {
        destination =
            makeScratch(*commands, static_cast<UINT64>(pitch) * plan.height);
        offset = 0;

        if (destination == nullptr)
            return;
    }

    transitionTextureForUse(list, *texture, D3D12_RESOURCE_STATE_COPY_SOURCE);

    const auto from = subresourceLocation(*texture, plan.level, plan.slice);
    const auto to = footprintLocation(destination, *texture, offset, plan, pitch);
    const auto box = boxOf(plan.x, plan.y, plan.width, plan.height);

    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);

    restAfterCopy(list, *texture);

    if (aligned)
        return;

    transition(list,
               destination,
               D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionForUse(*commands, *buffer, D3D12_RESOURCE_STATE_COPY_DEST);

    copyRows(list,
             buffer->resource.get(),
             static_cast<UINT64>(plan.bufferOffset),
             static_cast<UINT64>(plan.bytesPerRow),
             destination,
             0,
             pitch,
             plan);
}

namespace
{
UINT groupsFor(int size, int groupSize)
{
    return static_cast<UINT>((size + groupSize - 1) / groupSize);
}

int mipKernelKind(const Texture& texture)
{
    switch (texture.kind())
    {
        case TextureType::TwoD:
            return 0;
        case TextureType::ThreeD:
            return 2;
        case TextureType::Cube:
        case TextureType::TwoDArray:
        case TextureType::CubeArray:
            break;
    }

    return 1;
}
} // namespace

// A dispatch per level, reading the level above through its UAV and writing
// this one through its own, with a UAV barrier between levels so each reads
// what the last wrote. The whole resource is in UNORDERED_ACCESS for it and
// goes back to PIXEL_SHADER_RESOURCE after, as a copy leaves it.
void recordMipGeneration(void* recording, const Texture& texture)
{
    auto* commands = static_cast<CommandContext*>(recording);
    auto* data = textureDataOf(texture);

    if (commands == nullptr || commands->context == nullptr || data == nullptr)
        return;

    const auto levels = texture.mipLevels();

    if (static_cast<int>(data->levelUavs.size()) < levels)
    {
        LOG("D3D12: generateMipmaps on a texture created without per-level UAVs "
            "is dropped");
        return;
    }

    const auto kind = mipKernelKind(texture);
    auto* state = getD3D12Shared().getMipShader(kind);

    if (state == nullptr)
        return;

    auto* list = commands->list.get();

    bindComputeRootState(*commands->context, list);
    list->SetPipelineState(state);

    transitionTextureForUse(list, *data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    for (auto level = 1; level < levels; ++level)
    {
        const auto& source = data->levelUavs[(std::size_t) (level - 1)];
        const auto& target = data->levelUavs[(std::size_t) level];

        if (source.gpu.ptr == 0 || target.gpu.ptr == 0)
            break;

        list->SetComputeRootDescriptorTable(computeTextureUAVParam(0), source.gpu);
        list->SetComputeRootDescriptorTable(computeTextureUAVParam(1), target.gpu);

        const auto width = levelWidth(texture, level);
        const auto height = levelHeight(texture, level);

        if (kind == 2)
            list->Dispatch(groupsFor(width, 4),
                           groupsFor(height, 4),
                           groupsFor(levelSlices(texture, level), 4));
        else
            list->Dispatch(groupsFor(width, 8),
                           groupsFor(height, 8),
                           static_cast<UINT>(texture.slices()));

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = data->resource.get();
        list->ResourceBarrier(1, &barrier);
    }

    restAfterCopy(list, *data);
}
} // namespace eacp::GPU
