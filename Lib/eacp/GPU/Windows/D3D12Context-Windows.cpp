#include <eacp/Core/Utils/WinInclude.h>
#include "../Common.h"

#include "D3D12Context.h"
#include "D3D12Types.h"

#include <algorithm>

namespace eacp::GPU
{
namespace
{
constexpr UINT textureHeapCapacity = 1024;
constexpr UINT samplerHeapCapacity = 256;

// Root CBVs read in 256-byte units, so transient constant uploads round up.
constexpr std::size_t constantAlignment = 256;

// Enough that a typical frame uses one chunk, created once and refilled.
constexpr std::size_t uploadChunkBytes = 1024 * 1024;

winrt::com_ptr<ID3D12Device> createHardwareOrWarpDevice()
{
    auto device = winrt::com_ptr<ID3D12Device>();

#ifndef NDEBUG
    // The SDK layers need Graphics Tools installed; failure just means no
    // validation.
    auto debug = winrt::com_ptr<ID3D12Debug>();
    if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), debug.put_void())))
        debug->EnableDebugLayer();
#endif

    if (SUCCEEDED(D3D12CreateDevice(nullptr,
                                    D3D_FEATURE_LEVEL_11_0,
                                    __uuidof(ID3D12Device),
                                    device.put_void())))
        return device;

    // Fallback to WARP software renderer (also the headless CI path).
    auto factory = winrt::com_ptr<IDXGIFactory4>();
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void())))
        return nullptr;

    auto warpAdapter = winrt::com_ptr<IDXGIAdapter>();
    if (FAILED(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter),
                                        warpAdapter.put_void())))
        return nullptr;

    D3D12CreateDevice(warpAdapter.get(),
                      D3D_FEATURE_LEVEL_11_0,
                      __uuidof(ID3D12Device),
                      device.put_void());
    return device;
}

D3D12_ROOT_PARAMETER rootCBV(UINT shaderRegister, D3D12_SHADER_VISIBILITY visibility)
{
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = shaderRegister;
    parameter.ShaderVisibility = visibility;
    return parameter;
}

// Visibility defaults to ALL, the only thing a compute signature takes; the
// render signature names a stage so one register can bind per stage.
D3D12_ROOT_PARAMETER
rootBufferView(D3D12_ROOT_PARAMETER_TYPE type,
               UINT shaderRegister,
               D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL)
{
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = type;
    parameter.Descriptor.ShaderRegister = shaderRegister;
    parameter.ShaderVisibility = visibility;
    return parameter;
}

// Visibility is a parameter because a compute root signature takes only ALL:
// naming a stage in one fails serialisation rather than narrowing.
D3D12_ROOT_PARAMETER
rootTable(const D3D12_DESCRIPTOR_RANGE* range,
          D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL)
{
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = range;
    parameter.ShaderVisibility = visibility;
    return parameter;
}

winrt::com_ptr<ID3D12RootSignature>
    makeRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc)
{
    auto blob = winrt::com_ptr<ID3DBlob>();
    auto errors = winrt::com_ptr<ID3DBlob>();

    if (FAILED(D3D12SerializeRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put())))
        return nullptr;

    auto signature = winrt::com_ptr<ID3D12RootSignature>();
    device->CreateRootSignature(0,
                                blob->GetBufferPointer(),
                                blob->GetBufferSize(),
                                __uuidof(ID3D12RootSignature),
                                signature.put_void());
    return signature;
}
} // namespace

D3D12Context::D3D12Context()
{
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    createAll();
}

D3D12Context::~D3D12Context()
{
    if (isValid())
        waitIdle();

    if (fenceEvent != nullptr)
        CloseHandle(fenceEvent);
}

void D3D12Context::createAll()
{
    createDevice();

    if (device == nullptr)
        return;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (FAILED(device->CreateCommandQueue(
            &queueDesc, __uuidof(ID3D12CommandQueue), queue.put_void()))
        || FAILED(device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), fence.put_void())))
    {
        fence = nullptr;
        queue = nullptr;
        device = nullptr;
        return;
    }

    nextFenceValue = 1;
    lastSubmittedValue = 0;

    createRootSignatures();

    textureDescriptors = makeDescriptorAllocator(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, textureHeapCapacity);
    samplerDescriptors = makeDescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                                 samplerHeapCapacity);

    createNullDescriptors();
}

// Tier 1 hardware requires every declared descriptor table populated, and the
// binding must stay valid: a recycled heap slot can come to describe a
// destroyed resource, and binding that hangs the device. Never freed.
void D3D12Context::createNullDescriptors()
{
    if (device == nullptr)
        return;

    nullTexture = allocateFrom(textureDescriptors);
    nullTextureUAV = allocateFrom(textureDescriptors);
    nullSampler = allocateFrom(samplerDescriptors);

    if (nullTexture.cpu.ptr != 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(nullptr, &srv, nullTexture.cpu);
    }

    // A UAV range will not take the SRV descriptor above, so the compute
    // signature's UAV tables need their own.
    if (nullTextureUAV.cpu.ptr != 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            nullptr, nullptr, &uav, nullTextureUAV.cpu);
    }

    if (nullSampler.cpu.ptr != 0)
    {
        D3D12_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;

        device->CreateSampler(&sampler, nullSampler.cpu);
    }
}

void D3D12Context::createDevice()
{
    device = createHardwareOrWarpDevice();
}

void D3D12Context::createRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE srvRanges[maxTextureSlots] = {};
    D3D12_ROOT_PARAMETER
    renderParams[2 * maxUniformSlots + maxTextureSlots + 2 * maxBufferSlots];

    for (auto slot = 0; slot < maxUniformSlots; ++slot)
    {
        renderParams[renderVertexCBVParam(slot)] =
            rootCBV(static_cast<UINT>(slot), D3D12_SHADER_VISIBILITY_VERTEX);
        renderParams[renderPixelCBVParam(slot)] =
            rootCBV(static_cast<UINT>(slot), D3D12_SHADER_VISIBILITY_PIXEL);
    }

    for (auto slot = 0; slot < maxTextureSlots; ++slot)
    {
        srvRanges[slot].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[slot].NumDescriptors = 1;
        srvRanges[slot].BaseShaderRegister = static_cast<UINT>(slot);

        renderParams[renderTextureParam(slot)] = rootTable(&srvRanges[slot]);
    }

    // Root descriptors rather than tables, at registers above every texture
    // slot, declared per stage at the same register.
    for (auto slot = 0; slot < maxBufferSlots; ++slot)
    {
        renderParams[renderVertexSRVParam(slot)] =
            rootBufferView(D3D12_ROOT_PARAMETER_TYPE_SRV,
                           renderBufferRegister(slot),
                           D3D12_SHADER_VISIBILITY_VERTEX);
        renderParams[renderPixelSRVParam(slot)] =
            rootBufferView(D3D12_ROOT_PARAMETER_TYPE_SRV,
                           renderBufferRegister(slot),
                           D3D12_SHADER_VISIBILITY_PIXEL);
    }

    // One per (texture slot, sampling configuration), at the register
    // ShaderEmitter emits. Static, not heap-bound: a Windows-on-Arm driver
    // ignores sampler table offsets. See TextureSampling.
    D3D12_STATIC_SAMPLER_DESC
    staticSamplers[maxTextureSlots * samplingConfigurations] = {};

    for (auto slot = 0; slot < maxTextureSlots; ++slot)
    {
        for (auto configuration = 0; configuration < samplingConfigurations;
             ++configuration)
        {
            const auto linear = (configuration & 2) != 0;
            const auto repeat = (configuration & 1) != 0;
            const auto address = repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP
                                        : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

            auto& sampler =
                staticSamplers[slot * samplingConfigurations + configuration];

            sampler.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                                    : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = address;
            sampler.AddressV = address;
            sampler.AddressW = address;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;

            // Zero is not a legal D3D12_COMPARISON_FUNC (they run 1..8), and
            // serialising the root signature validates static samplers.
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            sampler.ShaderRegister =
                static_cast<UINT>(slot * samplingConfigurations + configuration);
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
    }

    D3D12_ROOT_SIGNATURE_DESC renderDesc = {};
    renderDesc.NumParameters = static_cast<UINT>(std::size(renderParams));
    renderDesc.pParameters = renderParams;
    renderDesc.NumStaticSamplers = static_cast<UINT>(std::size(staticSamplers));
    renderDesc.pStaticSamplers = staticSamplers;
    renderDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    renderRootSignature = makeRootSignature(device.get(), renderDesc);

    D3D12_DESCRIPTOR_RANGE computeSrvRanges[maxTextureSlots] = {};
    D3D12_DESCRIPTOR_RANGE computeUavRanges[maxTextureSlots] = {};
    D3D12_ROOT_PARAMETER
    computeParams[maxUniformSlots + 2 * maxBufferSlots + 2 * maxTextureSlots];

    for (auto slot = 0; slot < maxUniformSlots; ++slot)
        computeParams[computeCBVParam(slot)] =
            rootCBV(static_cast<UINT>(slot), D3D12_SHADER_VISIBILITY_ALL);

    for (auto slot = 0; slot < maxBufferSlots; ++slot)
    {
        computeParams[computeSRVParam(slot)] =
            rootBufferView(D3D12_ROOT_PARAMETER_TYPE_SRV, static_cast<UINT>(slot));
        computeParams[computeUAVParam(slot)] =
            rootBufferView(D3D12_ROOT_PARAMETER_TYPE_UAV, static_cast<UINT>(slot));
    }

    // Root descriptors are buffer views, so a texture goes through a
    // single-descriptor table either direction, at registers above the buffers'
    // - the two slot spaces share t and u. See computeTextureRegister.
    for (auto slot = 0; slot < maxTextureSlots; ++slot)
    {
        computeSrvRanges[slot].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        computeSrvRanges[slot].NumDescriptors = 1;
        computeSrvRanges[slot].BaseShaderRegister = computeTextureRegister(slot);

        computeUavRanges[slot].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        computeUavRanges[slot].NumDescriptors = 1;
        computeUavRanges[slot].BaseShaderRegister = computeTextureRegister(slot);

        computeParams[computeTextureSRVParam(slot)] =
            rootTable(&computeSrvRanges[slot], D3D12_SHADER_VISIBILITY_ALL);
        computeParams[computeTextureUAVParam(slot)] =
            rootTable(&computeUavRanges[slot], D3D12_SHADER_VISIBILITY_ALL);
    }

    // The render signature's samplers at the same registers, so a kernel and a
    // fragment shader read identical emitted source. Only visibility differs, a
    // compute root signature accepting nothing but ALL.
    constexpr auto samplerCount = maxTextureSlots * samplingConfigurations;
    D3D12_STATIC_SAMPLER_DESC computeSamplers[samplerCount] = {};

    for (auto i = 0; i < samplerCount; ++i)
    {
        computeSamplers[i] = staticSamplers[i];
        computeSamplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC computeDesc = {};
    computeDesc.NumParameters = static_cast<UINT>(std::size(computeParams));
    computeDesc.pParameters = computeParams;
    computeDesc.NumStaticSamplers = static_cast<UINT>(std::size(computeSamplers));
    computeDesc.pStaticSamplers = computeSamplers;

    computeRootSignature = makeRootSignature(device.get(), computeDesc);
}

// pRootSignature stays null, which D3D12 allows exactly when every argument is
// a Draw or a Dispatch: no root-argument layout varies per command.
ID3D12CommandSignature* D3D12Context::getDispatchSignature()
{
    if (dispatchSignature != nullptr || device == nullptr)
        return dispatchSignature.get();

    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &argument;

    device->CreateCommandSignature(&desc,
                                   nullptr,
                                   __uuidof(ID3D12CommandSignature),
                                   dispatchSignature.put_void());

    return dispatchSignature.get();
}

D3D12Context::DescriptorAllocator
    D3D12Context::makeDescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                          UINT capacity)
{
    auto allocator = DescriptorAllocator();

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(
            &desc, __uuidof(ID3D12DescriptorHeap), allocator.heap.put_void())))
        return allocator;

    allocator.capacity = capacity;
    allocator.descriptorSize = device->GetDescriptorHandleIncrementSize(type);
    return allocator;
}

DescriptorSlot D3D12Context::allocateFrom(DescriptorAllocator& allocator)
{
    if (allocator.heap == nullptr)
        return {};

    auto index = UINT {0};

    if (!allocator.freeList.empty())
    {
        index = allocator.freeList.back();
        allocator.freeList.pop_back();
    }
    else if (allocator.next < allocator.capacity)
    {
        index = allocator.next++;
    }
    else
    {
        return {};
    }

    auto slot = DescriptorSlot();
    slot.index = index;
    slot.generation = generation;
    slot.cpu = allocator.heap->GetCPUDescriptorHandleForHeapStart();
    slot.cpu.ptr += static_cast<SIZE_T>(index) * allocator.descriptorSize;
    slot.gpu = allocator.heap->GetGPUDescriptorHandleForHeapStart();
    slot.gpu.ptr += static_cast<UINT64>(index) * allocator.descriptorSize;
    return slot;
}

void D3D12Context::freeFrom(DescriptorAllocator& allocator,
                            const DescriptorSlot& slot)
{
    // A slot from before device loss indexes heaps that no longer exist.
    if (slot.generation != generation || allocator.heap == nullptr)
        return;

    allocator.freeList.push_back(slot.index);
}

DescriptorSlot D3D12Context::allocateTextureDescriptor()
{
    return allocateFrom(textureDescriptors);
}

void D3D12Context::freeTextureDescriptor(const DescriptorSlot& slot)
{
    freeFrom(textureDescriptors, slot);
}

DescriptorSlot D3D12Context::allocateSamplerDescriptor()
{
    return allocateFrom(samplerDescriptors);
}

void D3D12Context::freeSamplerDescriptor(const DescriptorSlot& slot)
{
    freeFrom(samplerDescriptors, slot);
}

void D3D12Context::deferReleaseUnknown(winrt::com_ptr<IUnknown> object)
{
    if (object == nullptr)
        return;

    // Unstamped: an open list may still name the object and has no fence value
    // until it submits, so there is nothing sound to stamp with until nothing
    // is recording. purgeRetired does that.
    retired.add({std::move(object), 0, false});
}

void D3D12Context::purgeRetired()
{
    // Nothing recording means every list that could name a retired object has
    // submitted, so lastSubmittedValue is a sound stamp. Called from the top of
    // acquire(), before the caller's context leaves the pool.
    if (available.size() == pool.size())
    {
        for (auto& entry: retired)
        {
            if (!entry.stamped)
            {
                entry.fenceValue = lastSubmittedValue;
                entry.stamped = true;
            }
        }
    }

    // Each goes as its own value passes: waiting for the fence to pass
    // everything ever submitted frees nothing under continuous rendering.
    retired.eraseIf([this](const Retired& entry)
                    { return entry.stamped && hasCompleted(entry.fenceValue); });

    releaseRecycledBuffers();
}

// purgeRetired's two steps, for buffers handed out again rather than dropped.
void D3D12Context::releaseRecycledBuffers()
{
    if (available.size() == pool.size())
    {
        for (auto& entry: recycling)
        {
            if (!entry.stamped)
            {
                entry.fenceValue = lastSubmittedValue;
                entry.stamped = true;
            }
        }
    }

    recycling.eraseIf(
        [this](PooledBuffer& entry)
        {
            if (!entry.stamped || !hasCompleted(entry.fenceValue))
                return false;

            if (reusableBytes + entry.capacity <= reusableBudget)
            {
                reusableBytes += entry.capacity;
                reusable.add(std::move(entry));
            }

            return true;
        });
}

winrt::com_ptr<ID3D12Resource>
    D3D12Context::takeDefaultBuffer(std::size_t bytes, D3D12_RESOURCE_FLAGS flags)
{
    // Best fit, so a small request does not take the megabyte-sized spare.
    auto best = reusable.end();

    for (auto it = reusable.begin(); it != reusable.end(); ++it)
        if (it->flags == flags && it->capacity >= bytes)
            if (best == reusable.end() || it->capacity < best->capacity)
                best = it;

    if (best == reusable.end())
        return nullptr;

    auto resource = std::move(best->resource);
    reusableBytes -= best->capacity;
    reusable.erase(best);

    return resource;
}

void D3D12Context::recycleDefaultBuffer(winrt::com_ptr<ID3D12Resource> resource,
                                        std::size_t capacity,
                                        D3D12_RESOURCE_FLAGS flags)
{
    if (resource == nullptr)
        return;

    auto entry = PooledBuffer {};
    entry.resource = std::move(resource);
    entry.capacity = capacity;
    entry.flags = flags;

    recycling.add(std::move(entry));
}

CommandContext* D3D12Context::acquire()
{
    if (!isValid())
        return nullptr;

    purgeRetired();

    auto recycled = std::find_if(available.begin(),
                                 available.end(),
                                 [this](CommandContext* candidate)
                                 { return hasCompleted(candidate->fenceValue); });

    CommandContext* commands = nullptr;

    if (recycled != available.end())
    {
        commands = *recycled;
        available.erase(recycled);
        commands->transients.clear();
        commands->rewindUploads();
        commands->allocator->Reset();
        commands->list->Reset(commands->allocator.get(), nullptr);
    }
    else
    {
        auto fresh = makeOwned<CommandContext>();

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  __uuidof(ID3D12CommandAllocator),
                                                  fresh->allocator.put_void()))
            || FAILED(device->CreateCommandList(0,
                                                D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                fresh->allocator.get(),
                                                nullptr,
                                                __uuidof(ID3D12GraphicsCommandList),
                                                fresh->list.put_void())))
            return nullptr;

        commands = fresh.get();
        pool.add(std::move(fresh));
    }

    commands->fenceValue = 0;
    commands->recordingId = ++recordingCounter;
    return commands;
}

std::uint64_t D3D12Context::submit(CommandContext* commands)
{
    if (commands == nullptr || !isValid())
        return 0;

    if (FAILED(commands->list->Close()))
    {
        // Nothing reached the GPU, so the staging slots are free at once.
        commands->transients.clear();
        returnStaging(*commands, 0);
        available.push_back(commands);
        return 0;
    }

    ID3D12CommandList* lists[] = {commands->list.get()};
    queue->ExecuteCommandLists(1, lists);

    commands->fenceValue = signal();
    lastSubmittedValue = commands->fenceValue;
    returnStaging(*commands, commands->fenceValue);
    available.push_back(commands);
    return commands->fenceValue;
}

void D3D12Context::discard(CommandContext* commands)
{
    if (commands == nullptr)
        return;

    commands->list->Close();
    commands->transients.clear();
    returnStaging(*commands, 0);
    commands->fenceValue = 0;
    available.push_back(commands);
}

std::uint64_t D3D12Context::signal()
{
    auto value = nextFenceValue++;
    queue->Signal(fence.get(), value);
    return value;
}

bool D3D12Context::hasCompleted(std::uint64_t value) const
{
    if (fence == nullptr)
        return true;

    // On device removal the completed value jumps to UINT64_MAX, unblocking
    // every wait so recovery can proceed.
    return fence->GetCompletedValue() >= value;
}

void D3D12Context::waitFor(std::uint64_t value)
{
    if (fence == nullptr || fenceEvent == nullptr || hasCompleted(value))
        return;

    if (SUCCEEDED(fence->SetEventOnCompletion(value, fenceEvent)))
        WaitForSingleObject(fenceEvent, INFINITE);
}

void D3D12Context::waitIdle()
{
    if (!isValid())
        return;

    waitFor(signal());
}

void D3D12Context::notifyWhenCompleted(std::uint64_t value, Callback done)
{
    if (hasCompleted(value))
    {
        done();
        return;
    }

    pendingCompletions.add({value, std::move(done)});

    if (!completionPoll.has_value())
        completionPoll.emplace([this] { pollCompletions(); }, completionPollHz);
}

void D3D12Context::pollCompletions()
{
    // Fired after the pending list is rebuilt, a callback being free to commit
    // more work and append to the vector being walked.
    auto ready = Vector<Callback> {};
    auto stillPending = Vector<PendingCompletion> {};

    for (auto& pending: pendingCompletions)
    {
        if (hasCompleted(pending.fenceValue))
            ready.add(std::move(pending.done));
        else
            stillPending.add(std::move(pending));
    }

    pendingCompletions = std::move(stillPending);

    if (pendingCompletions.empty())
        completionPoll.reset();

    for (auto& done: ready)
        done();
}

CommandContext::UploadChunk* D3D12Context::uploadRoomFor(CommandContext& commands,
                                                         std::size_t bytes)
{
    // Forward only: rechecking passed chunks would make this linear in the
    // uploads a frame has already made.
    while (commands.uploadCursor < commands.uploads.size())
    {
        auto& chunk = commands.uploads[commands.uploadCursor];

        if (chunk.capacity - chunk.used >= bytes)
            return &chunk;

        ++commands.uploadCursor;
    }

    auto chunk = CommandContext::UploadChunk {};
    chunk.capacity = std::max(uploadChunkBytes, bytes);
    chunk.resource = makeUploadBuffer(nullptr, chunk.capacity);

    if (chunk.resource == nullptr)
        return nullptr;

    // Left mapped for life: an upload heap is CPU-visible memory, so mapping
    // per write is a page-table round trip for a pointer that never changes.
    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};

    if (FAILED(chunk.resource->Map(0, &noRead, &mapped)))
        return nullptr;

    chunk.mapped = static_cast<std::uint8_t*>(mapped);

    return &commands.uploads.add(std::move(chunk));
}

UploadRange D3D12Context::allocateUpload(CommandContext& commands, std::size_t bytes)
{
    // Always constant-aligned, so a copy source and a root CBV can share one
    // arena without an odd length pushing the next constant off its boundary.
    auto aligned = (bytes + constantAlignment - 1) & ~(constantAlignment - 1);
    auto* chunk = uploadRoomFor(commands, aligned);

    if (chunk == nullptr)
        return {};

    auto range = UploadRange {};
    range.resource = chunk->resource.get();
    range.mapped = chunk->mapped + chunk->used;
    range.offset = chunk->used;
    range.address = chunk->resource->GetGPUVirtualAddress() + chunk->used;

    chunk->used += aligned;

    return range;
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Context::uploadConstants(CommandContext& commands,
                                                        const void* data,
                                                        std::size_t bytes)
{
    auto range = allocateUpload(commands, bytes);

    if (!range.isValid())
        return 0;

    std::memcpy(range.mapped, data, bytes);

    return range.address;
}

ID3D12Resource* D3D12Context::acquireStagingBuffer(CommandContext& commands,
                                                   std::size_t bytes)
{
    if (!isValid() || bytes == 0)
        return nullptr;

    auto isFree = [this](const StagingBuffer& slot)
    { return !slot.lent && hasCompleted(slot.freeAt); };

    for (auto index = 0; index < staging.size(); ++index)
    {
        auto& slot = staging[index];

        if (isFree(slot) && slot.bytes >= bytes)
        {
            slot.lent = true;
            commands.stagingTaken.add(index);
            return slot.resource.get();
        }
    }

    // Grow a free slot rather than adding one, so a stream switching to a
    // larger frame size does not strand the old buffers.
    for (auto index = 0; index < staging.size(); ++index)
    {
        auto& slot = staging[index];

        if (!isFree(slot))
            continue;

        auto grown = makeUploadBuffer(nullptr, bytes);

        if (grown == nullptr)
            return nullptr;

        deferRelease(std::move(slot.resource));
        slot.resource = std::move(grown);
        slot.bytes = bytes;
        slot.lent = true;
        commands.stagingTaken.add(index);
        return slot.resource.get();
    }

    auto fresh = makeUploadBuffer(nullptr, bytes);

    if (fresh == nullptr)
        return nullptr;

    auto slot = StagingBuffer {};
    slot.resource = std::move(fresh);
    slot.bytes = bytes;
    slot.lent = true;

    auto* resource = slot.resource.get();
    staging.add(std::move(slot));
    commands.stagingTaken.add(staging.size() - 1);
    return resource;
}

void D3D12Context::returnStaging(CommandContext& commands, std::uint64_t freeAt)
{
    for (auto index: commands.stagingTaken)
    {
        if (index < 0 || index >= staging.size())
            continue;

        staging[index].lent = false;
        staging[index].freeAt = freeAt;
    }

    commands.stagingTaken.clear();
}

winrt::com_ptr<ID3D12Resource> D3D12Context::makeUploadBuffer(const void* data,
                                                              std::size_t bytes)
{
    if (!isValid() || bytes == 0)
        return nullptr;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto buffer = winrt::com_ptr<ID3D12Resource>();

    if (FAILED(device->CreateCommittedResource(&heap,
                                               D3D12_HEAP_FLAG_NONE,
                                               &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr,
                                               __uuidof(ID3D12Resource),
                                               buffer.put_void())))
        return nullptr;

    if (data != nullptr)
    {
        void* mapped = nullptr;
        const D3D12_RANGE noRead = {0, 0};

        if (FAILED(buffer->Map(0, &noRead, &mapped)))
            return nullptr;

        std::memcpy(mapped, data, bytes);
        buffer->Unmap(0, nullptr);
    }

    return buffer;
}

void D3D12Context::recreateAfterDeviceLoss()
{
    retired.clear();
    staging.clear();
    recycling.clear();
    reusable.clear();
    reusableBytes = 0;
    openRecording = nullptr;
    pool.clear();
    available.clear();
    renderRootSignature = nullptr;
    computeRootSignature = nullptr;
    dispatchSignature = nullptr;
    textureDescriptors = {};
    samplerDescriptors = {};
    fence = nullptr;
    queue = nullptr;
    device = nullptr;

    ++generation;
    createAll();
}

D3D12Context& getD3D12Context()
{
    static auto context = D3D12Context();
    return context;
}
} // namespace eacp::GPU
