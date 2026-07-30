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

// One page of the constant ring, sized so a recording of a few hundred
// dispatches or draws fits in a single page and never allocates mid-frame.
constexpr std::size_t constantPageBytes = 64 * 1024;

winrt::com_ptr<ID3D12Device> createHardwareOrWarpDevice()
{
    auto device = winrt::com_ptr<ID3D12Device>();

#ifndef NDEBUG
    // Best effort: the SDK layers are only present with Graphics Tools
    // installed, so a failure here just means no validation.
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

// The visibility defaults to ALL for the compute signature, which accepts
// nothing else; the render signature names a stage, the way its CBVs do, so one
// register can carry a separate binding per stage.
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

// The visibility is a parameter rather than baked in because a compute root
// signature takes only ALL: naming a stage in one is not a narrowing, it is a
// serialisation failure, and the whole signature is refused.
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

// Tier 1 hardware requires every descriptor table the root signature declares
// to be populated, so the slots a shader does not use still need something
// bound. It has to be something permanently valid: the obvious candidate — the
// heap's first descriptor — belongs to whichever texture allocated it, and
// descriptor slots are recycled through a free list, so that descriptor can
// come to describe a destroyed resource. Binding it then points the GPU at
// freed memory, which hangs the device rather than failing cleanly.
//
// A null SRV is the case D3D12 provides for exactly this: reads return zero
// and nothing is dereferenced. These two slots are allocated once and never
// freed.
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

    // The compute signature's UAV tables need their own: a range declared UAV
    // will not take the SRV descriptor above, so the two are separate slots
    // even though both describe nothing.
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

    // The storage buffers a vertex or fragment stage subscripts. Root
    // descriptors rather than tables, at registers above every texture slot,
    // and declared per stage at the same register - one HLSL global is visible
    // to both functions, so each stage binds its own.
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

    // A static sampler for every (texture slot, sampling configuration) pair, at
    // register s(slot * samplingConfigurations + configuration) - the register
    // ShaderEmitter points each texture's sampler at.
    //
    // Samplers are declared here rather than bound per draw from a descriptor
    // heap because a sampler descriptor table cannot be relied on: a
    // Windows-on-Arm driver ignores the table's offset and resolves every sampler
    // to descriptor 0 of the bound heap, so all textures in the process sample
    // through whichever sampler happens to be first. Static samplers never reach
    // a heap and are unaffected. See TextureSampling.
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

            // Not left at 0: zero is not a legal D3D12_COMPARISON_FUNC (they run
            // 1..8), and a static sampler is validated when the root signature is
            // serialised rather than quietly ignored.
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

    // A texture is not a root descriptor on either side of the read/write
    // split: root descriptors are buffer views, so both go through
    // single-descriptor tables. Their registers start above the buffers' — the
    // two slot spaces share t and u. See computeTextureRegister.
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

    // The same static samplers the render signature declares, at the same
    // registers, so a kernel sampling a texture and a fragment shader sampling
    // one read the identical emitted source. Only the visibility differs, for
    // the reason rootTable takes one: a compute root signature accepts nothing
    // but ALL.
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

    // Stamped with the value the next signal will carry: by the time that
    // value completes, everything submitted before this call — and the open
    // recording that submits next — has finished with the object. purgeRetired
    // additionally waits for every recording to close; see the note there.
    retired.add({std::move(object), nextFenceValue});
}

void D3D12Context::purgeRetired()
{
    // Freed only when the GPU has gone completely idle: nothing left recording,
    // and the fence past everything ever submitted.
    //
    // The per-entry fence value cannot answer this on its own. A retired object
    // is stamped with the value the *next* signal will carry, which assumes the
    // recording that references it submits next — but a frame issues uploads of
    // its own (every setInstances makes a buffer), and each acquires a second
    // list that signals *ahead* of the frame. The frame's list then submits with
    // a higher value than the stamp, so the stamp completes while the list still
    // referencing the object is either open or still executing. Releasing there
    // is a use-after-free the debug layer raises on, and it crashed the editor
    // a few keystrokes in — every keystroke rebuilds the glyph instance buffer,
    // so this path runs constantly once text is on screen.
    //
    // Draining is coarse but provably safe, and costs nothing here: the retired
    // list holds a frame's worth of buffers, and the GPU goes idle between
    // frames in an editor that only redraws on input.
    if (available.size() != pool.size() || !hasCompleted(lastSubmittedValue))
        return;

    retired.clear();
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
        // An invalid recording (or removed device) must not execute; recycle
        // the context with its transients released. Nothing reached the GPU, so
        // its staging slots are free at once rather than behind a fence.
        commands->transients.clear();
        returnStaging(*commands, 0);
        returnConstantPages(*commands, 0);
        available.push_back(commands);
        return 0;
    }

    ID3D12CommandList* lists[] = {commands->list.get()};
    queue->ExecuteCommandLists(1, lists);

    commands->fenceValue = signal();
    lastSubmittedValue = commands->fenceValue;
    returnStaging(*commands, commands->fenceValue);
    returnConstantPages(*commands, commands->fenceValue);
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
    returnConstantPages(*commands, 0);
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

    // On device removal the completed value jumps to UINT64_MAX, so every
    // wait unblocks and recovery can proceed.
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
    // The callbacks fire after the pending list has been rebuilt rather than
    // during the walk: one of them is free to commit more work, which appends
    // to the very vector being walked.
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

D3D12_GPU_VIRTUAL_ADDRESS D3D12Context::uploadConstants(CommandContext& commands,
                                                        const void* data,
                                                        std::size_t bytes)
{
    if (data == nullptr || bytes == 0)
        return 0;

    auto aligned = (bytes + constantAlignment - 1) & ~(constantAlignment - 1);
    auto* page = pageFor(commands, aligned);

    if (page == nullptr)
        return 0;

    auto offset = page->used;
    page->used += aligned;

    std::memcpy(page->mapped + offset, data, bytes);
    return page->address + offset;
}

D3D12Context::ConstantPage* D3D12Context::pageFor(CommandContext& commands,
                                                  std::size_t bytes)
{
    // The page this recording is already filling, while it still has room.
    // Constant blocks are 256 bytes and a recording's dispatches run into the
    // hundreds at most, so this is the answer nearly every time.
    if (!commands.constantsTaken.empty())
    {
        auto lastTaken =
            commands.constantsTaken[commands.constantsTaken.getLastElementIndex()];
        auto& open = constantPages[lastTaken];

        if (open.remaining() >= bytes)
            return &open;
    }

    auto take = [&](int index) -> ConstantPage*
    {
        auto& page = constantPages[index];
        page.lent = true;
        page.used = 0;
        commands.constantsTaken.add(index);
        return &page;
    };

    for (auto index = 0; index < constantPages.size(); ++index)
    {
        const auto& page = constantPages[index];

        if (!page.lent && hasCompleted(page.freeAt) && page.bytes >= bytes)
            return take(index);
    }

    // A block larger than the page size gets a page of its own rather than
    // failing — uniform blocks are capped well below it, but nothing here
    // depends on that being true.
    auto pageBytes = std::max(constantPageBytes, bytes);
    auto resource = makeUploadBuffer(nullptr, pageBytes);

    if (resource == nullptr)
        return nullptr;

    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};

    if (FAILED(resource->Map(0, &noRead, &mapped)))
        return nullptr;

    auto page = ConstantPage {};
    page.address = resource->GetGPUVirtualAddress();
    page.mapped = static_cast<std::byte*>(mapped);
    page.bytes = pageBytes;
    page.resource = std::move(resource);

    constantPages.add(std::move(page));
    return take(constantPages.size() - 1);
}

ID3D12Resource* D3D12Context::acquirePooled(Vector<StagingBuffer>& pool,
                                            Vector<int>& taken,
                                            std::size_t bytes,
                                            D3D12_HEAP_TYPE heapType)
{
    if (!isValid() || bytes == 0)
        return nullptr;

    auto isFree = [this](const StagingBuffer& slot)
    { return !slot.lent && hasCompleted(slot.freeAt); };

    // A free slot already big enough is the common case once the traffic
    // settles: every frame of a given clip, and every run of a given model,
    // moves exactly the same number of bytes.
    for (auto index = 0; index < pool.size(); ++index)
    {
        auto& slot = pool[index];

        if (isFree(slot) && slot.bytes >= bytes)
        {
            slot.lent = true;
            taken.add(index);
            return slot.resource.get();
        }
    }

    // Otherwise grow a free slot rather than adding one, so a stream that
    // switches to a larger frame size does not strand the old buffers.
    for (auto index = 0; index < pool.size(); ++index)
    {
        auto& slot = pool[index];

        if (!isFree(slot))
            continue;

        auto grown = makeHeapBuffer(heapType, bytes);

        if (grown == nullptr)
            return nullptr;

        deferRelease(std::move(slot.resource));
        slot.resource = std::move(grown);
        slot.bytes = bytes;
        slot.lent = true;
        taken.add(index);
        return slot.resource.get();
    }

    auto fresh = makeHeapBuffer(heapType, bytes);

    if (fresh == nullptr)
        return nullptr;

    auto slot = StagingBuffer {};
    slot.resource = std::move(fresh);
    slot.bytes = bytes;
    slot.lent = true;

    auto* resource = slot.resource.get();
    pool.add(std::move(slot));
    taken.add(pool.size() - 1);
    return resource;
}

ID3D12Resource* D3D12Context::acquireStagingBuffer(CommandContext& commands,
                                                   std::size_t bytes)
{
    return acquirePooled(
        staging, commands.stagingTaken, bytes, D3D12_HEAP_TYPE_UPLOAD);
}

ID3D12Resource* D3D12Context::acquireReadbackBuffer(CommandContext& commands,
                                                    std::size_t bytes)
{
    return acquirePooled(
        readback, commands.readbackTaken, bytes, D3D12_HEAP_TYPE_READBACK);
}

void D3D12Context::returnPooled(Vector<StagingBuffer>& pool,
                                Vector<int>& taken,
                                std::uint64_t freeAt)
{
    for (auto index: taken)
    {
        if (index < 0 || index >= pool.size())
            continue;

        pool[index].lent = false;
        pool[index].freeAt = freeAt;
    }

    taken.clear();
}

void D3D12Context::returnStaging(CommandContext& commands, std::uint64_t freeAt)
{
    returnPooled(staging, commands.stagingTaken, freeAt);
    returnPooled(readback, commands.readbackTaken, freeAt);
}

void D3D12Context::returnConstantPages(CommandContext& commands,
                                       std::uint64_t freeAt)
{
    for (auto index: commands.constantsTaken)
    {
        if (index < 0 || index >= constantPages.size())
            continue;

        constantPages[index].lent = false;
        constantPages[index].freeAt = freeAt;
    }

    commands.constantsTaken.clear();
}

winrt::com_ptr<ID3D12Resource> D3D12Context::makeHeapBuffer(D3D12_HEAP_TYPE type,
                                                            std::size_t bytes)
{
    if (!isValid() || bytes == 0)
        return nullptr;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // The one state each heap type is created in and stays in: an upload
    // buffer is only ever read by the GPU, a readback buffer only ever written
    // by a copy.
    auto state = type == D3D12_HEAP_TYPE_READBACK
                     ? D3D12_RESOURCE_STATE_COPY_DEST
                     : D3D12_RESOURCE_STATE_GENERIC_READ;

    auto buffer = winrt::com_ptr<ID3D12Resource>();

    if (FAILED(device->CreateCommittedResource(&heap,
                                               D3D12_HEAP_FLAG_NONE,
                                               &desc,
                                               state,
                                               nullptr,
                                               __uuidof(ID3D12Resource),
                                               buffer.put_void())))
        return nullptr;

    return buffer;
}

winrt::com_ptr<ID3D12Resource> D3D12Context::makeUploadBuffer(const void* data,
                                                              std::size_t bytes)
{
    auto buffer = makeHeapBuffer(D3D12_HEAP_TYPE_UPLOAD, bytes);

    if (buffer == nullptr)
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
    readback.clear();
    constantPages.clear();
    pool.clear();
    available.clear();
    renderRootSignature = nullptr;
    computeRootSignature = nullptr;
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
