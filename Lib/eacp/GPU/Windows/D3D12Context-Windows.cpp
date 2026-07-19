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

D3D12_ROOT_PARAMETER rootBufferView(D3D12_ROOT_PARAMETER_TYPE type,
                                    UINT shaderRegister)
{
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = type;
    parameter.Descriptor.ShaderRegister = shaderRegister;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return parameter;
}

D3D12_ROOT_PARAMETER rootTable(const D3D12_DESCRIPTOR_RANGE* range)
{
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
    D3D12_DESCRIPTOR_RANGE samplerRanges[maxTextureSlots] = {};
    D3D12_ROOT_PARAMETER renderParams[2 * maxUniformSlots + 2 * maxTextureSlots];

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

        samplerRanges[slot].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRanges[slot].NumDescriptors = 1;
        samplerRanges[slot].BaseShaderRegister = static_cast<UINT>(slot);

        renderParams[renderTextureParam(slot)] = rootTable(&srvRanges[slot]);
        renderParams[renderSamplerParam(slot)] = rootTable(&samplerRanges[slot]);
    }

    D3D12_ROOT_SIGNATURE_DESC renderDesc = {};
    renderDesc.NumParameters = static_cast<UINT>(std::size(renderParams));
    renderDesc.pParameters = renderParams;
    renderDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    renderRootSignature = makeRootSignature(device.get(), renderDesc);

    D3D12_ROOT_PARAMETER computeParams[maxUniformSlots + 2 * maxBufferSlots];

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

    D3D12_ROOT_SIGNATURE_DESC computeDesc = {};
    computeDesc.NumParameters = static_cast<UINT>(std::size(computeParams));
    computeDesc.pParameters = computeParams;

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
        // the context with its transients released.
        commands->transients.clear();
        available.push_back(commands);
        return 0;
    }

    ID3D12CommandList* lists[] = {commands->list.get()};
    queue->ExecuteCommandLists(1, lists);

    commands->fenceValue = signal();
    lastSubmittedValue = commands->fenceValue;
    available.push_back(commands);
    return commands->fenceValue;
}

void D3D12Context::discard(CommandContext* commands)
{
    if (commands == nullptr)
        return;

    commands->list->Close();
    commands->transients.clear();
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

D3D12_GPU_VIRTUAL_ADDRESS D3D12Context::uploadConstants(CommandContext& commands,
                                                        const void* data,
                                                        std::size_t bytes)
{
    auto aligned = (bytes + constantAlignment - 1) & ~(constantAlignment - 1);
    auto buffer = makeUploadBuffer(nullptr, aligned);

    if (buffer == nullptr)
        return 0;

    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};

    if (FAILED(buffer->Map(0, &noRead, &mapped)))
        return 0;

    std::memcpy(mapped, data, bytes);
    buffer->Unmap(0, nullptr);

    auto address = buffer->GetGPUVirtualAddress();
    commands.transients.add(std::move(buffer));
    return address;
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
