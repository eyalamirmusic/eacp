#include <eacp/Core/Utils/WinInclude.h>
#include "../Common.h"

#include "D3D12Context.h"
#include "D3D12Types.h"

#include "../Device/Device.h"

#include <eacp/Core/Threads/ThreadUtils.h>
#include <eacp/Core/Utils/Strings.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eacp::GPU
{
namespace
{
// How many textures can exist at once. It is a descriptor each, held for the
// texture's lifetime, and the heap is shader-visible so it cannot be grown
// without invalidating every GPU handle already handed out - which is why the
// number is generous rather than tuned. 64K descriptors is 2 MB of heap on
// every device eacp runs on, and the count it replaces was 1024: a game level
// is a couple of thousand textures (Doom 3's first map is about 2,100), so the
// old ceiling was one an app could reach by loading its content.
//
// Reaching it is not graceful even now - allocateFrom below returns an invalid
// slot, which it logs - so the number matters.
constexpr UINT textureHeapCapacity = 65536;
constexpr UINT samplerHeapCapacity = 256;

// Root CBVs read in 256-byte units, so transient constant uploads round up.
constexpr std::size_t constantAlignment = 256;

// One page of the constant ring, sized so a recording of a few hundred
// dispatches or draws fits in a single page and never allocates mid-frame.
constexpr std::size_t constantPageBytes = 64 * 1024;

// How much upload space a recording is given at a time. A frame of a component
// interface uses well under a megabyte between its uniforms and its instance
// data, so the common case is one chunk created once and refilled for the rest
// of the run; the frame that builds a path-heavy interface's masks takes a
// handful more and then stops.
constexpr std::size_t uploadChunkBytes = 1024 * 1024;

// Whether an EACP_D3D12_* environment switch is set to anything but "0".
bool environmentFlag(const char* name)
{
    std::size_t length = 0;
    char value[8] = {};

    return getenv_s(&length, value, sizeof(value), name) == 0 && length > 1
           && value[0] != '0';
}

// EACP_D3D12_WARP=1 skips the hardware adapter and takes the software one. It
// is a debugging affordance and worth the four lines: WARP is the reference
// implementation, so an app that misbehaves on a GPU and behaves on WARP has
// found a driver bug rather than its own, and that is otherwise an expensive
// thing to establish. Slow enough that it is only ever a question being
// answered, never a way to run.
bool forcesWarp()
{
    return environmentFlag("EACP_D3D12_WARP");
}

winrt::com_ptr<ID3D12Resource> makeProbeResource(ID3D12Device* device,
                                                 const D3D12_RESOURCE_DESC& desc,
                                                 D3D12_RESOURCE_STATES state,
                                                 const D3D12_CLEAR_VALUE* clear)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                    ? D3D12_HEAP_TYPE_READBACK
                    : D3D12_HEAP_TYPE_DEFAULT;

    auto resource = winrt::com_ptr<ID3D12Resource>();
    device->CreateCommittedResource(&heap,
                                    D3D12_HEAP_FLAG_NONE,
                                    &desc,
                                    state,
                                    clear,
                                    __uuidof(ID3D12Resource),
                                    resource.put_void());
    return resource;
}

D3D12_RESOURCE_DESC probeTexture(UINT size,
                                 DXGI_FORMAT format,
                                 UINT samples,
                                 D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = samples;
    desc.Flags = flags;
    return desc;
}

// The resources a probe records against, held until the list has been closed:
// what it records is never executed, but the driver validates against them at
// Close(), which is the answer being asked for.
struct ProbeResources
{
    Vector<winrt::com_ptr<ID3D12Resource>> resources;
};

// The depth resolve exactly as resolveMultisampledTarget records it: a
// four-sample D32 plane into a single-sampled typeless twin, in RESOLVE_MODE_MAX.
bool recordDepthResolve(ID3D12Device* device,
                        ID3D12GraphicsCommandList* list,
                        ProbeResources& probe)
{
    constexpr auto size = UINT {8};
    constexpr auto samples = UINT {4};

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
    levels.Format = DXGI_FORMAT_D32_FLOAT;
    levels.SampleCount = samples;

    // A device without four-sample depth will never be asked to resolve one,
    // so there is nothing to find out.
    if (FAILED(device->CheckFeatureSupport(
            D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)))
        || levels.NumQualityLevels == 0)
        return false;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.f;

    auto multisampled =
        makeProbeResource(device,
                          probeTexture(size,
                                       DXGI_FORMAT_D32_FLOAT,
                                       samples,
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                          D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                          &clear);

    auto resolved =
        makeProbeResource(device,
                          probeTexture(size,
                                       DXGI_FORMAT_R32_TYPELESS,
                                       1,
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                          D3D12_RESOURCE_STATE_RESOLVE_DEST,
                          &clear);

    if (multisampled == nullptr || resolved == nullptr)
        return false;

    auto list1 = winrt::com_ptr<ID3D12GraphicsCommandList1>();

    if (FAILED(list->QueryInterface(__uuidof(ID3D12GraphicsCommandList1),
                                    list1.put_void())))
        return false;

    list1->ResolveSubresourceRegion(resolved.get(),
                                    0,
                                    0,
                                    0,
                                    multisampled.get(),
                                    0,
                                    nullptr,
                                    DXGI_FORMAT_R32_FLOAT,
                                    D3D12_RESOLVE_MODE_MAX);

    probe.resources.add(std::move(multisampled));
    probe.resources.add(std::move(resolved));
    return true;
}

// The region read-back exactly as Texture::read records it: a texture-to-buffer
// copy whose source box is a part of the texture rather than all of it.
bool recordBoxedTextureCopy(ID3D12Device* device,
                            ID3D12GraphicsCommandList* list,
                            ProbeResources& probe)
{
    constexpr auto size = UINT {4};
    constexpr auto regionSize = UINT {2};

    auto texture = makeProbeResource(
        device,
        probeTexture(size, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_NONE),
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        nullptr);

    auto regionDesc = probeTexture(
        regionSize, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_NONE);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(
        &regionDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = totalBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto buffer = makeProbeResource(
        device, bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);

    if (texture == nullptr || buffer == nullptr)
        return false;

    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = buffer.get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = texture.get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    const D3D12_BOX box = {1, 1, 0, 1 + regionSize, 1 + regionSize, 1};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);

    probe.resources.add(std::move(texture));
    probe.resources.add(std::move(buffer));
    return true;
}

// Whether the driver accepts a recording of `record`. Nothing is executed:
// both operations probed are refused at Close(), which is what makes this
// cheap enough to run at every device creation. A recording that could not be
// made - a resource the device would not create - is taken as accepted, there
// being nothing to refuse.
template <typename Record>
bool acceptsRecording(ID3D12Device* device, Record&& record)
{
    auto allocator = winrt::com_ptr<ID3D12CommandAllocator>();
    auto list = winrt::com_ptr<ID3D12GraphicsCommandList>();

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              __uuidof(ID3D12CommandAllocator),
                                              allocator.put_void()))
        || FAILED(device->CreateCommandList(0,
                                            D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            allocator.get(),
                                            nullptr,
                                            __uuidof(ID3D12GraphicsCommandList),
                                            list.put_void())))
        return true;

    auto probe = ProbeResources {};

    if (!record(device, list.get(), probe))
        return true;

    return SUCCEEDED(list->Close());
}

// Asks the hardware driver, on a device made for the purpose, whether it will
// take the two recordings the flags in DriverQuirks describe.
//
// By trial rather than by adapter name because a name is a guess about a
// driver and this is the driver's own answer: a fixed driver passes and loses
// the workaround by itself, and a different driver with the same gap is caught
// without anyone having heard of it. On a device made for the purpose because
// the trial is not survivable - the boxed copy removes the device it is
// recorded on, and the resolve leaves its command list unusable - so nothing
// the app will use may exist yet. Every object here is released before
// createHardwareOrWarpDevice runs, which is what lets a fresh device come up
// on the same adapter afterwards.
//
// EACP_D3D12_QUIRKS=1 sets every flag without asking, so the routes around
// them can be exercised on a driver that does not need them - WARP in CI,
// for one.
DriverQuirks probeDriverQuirks()
{
    auto quirks = DriverQuirks {};

    if (environmentFlag("EACP_D3D12_QUIRKS"))
    {
        quirks.noDepthResolve = true;
        quirks.noBoxedTextureCopy = true;
        return quirks;
    }

    if (forcesWarp())
        return quirks;

    auto device = winrt::com_ptr<ID3D12Device>();

    if (FAILED(D3D12CreateDevice(nullptr,
                                 D3D_FEATURE_LEVEL_11_0,
                                 __uuidof(ID3D12Device),
                                 device.put_void())))
        return quirks;

    quirks.noDepthResolve = !acceptsRecording(device.get(), recordDepthResolve);
    quirks.noBoxedTextureCopy =
        !acceptsRecording(device.get(), recordBoxedTextureCopy);

    if (quirks.noDepthResolve)
        LOG("D3D12: this driver refuses a multisampled depth resolve; "
            "resolving through a shader instead");

    if (quirks.noBoxedTextureCopy)
        LOG("D3D12: this driver refuses a boxed texture read-back; "
            "reading whole textures and cropping instead");

    return quirks;
}

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

    if (!forcesWarp()
        && SUCCEEDED(D3D12CreateDevice(nullptr,
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

// What the adapter this device came up on calls itself, for a log line or a
// benchmark header that has to say which GPU produced a number. Found by LUID
// rather than by re-running adapter selection, so it names the adapter
// D3D12CreateDevice actually picked - including WARP, which is worth seeing
// spelled out when a machine has silently fallen back to it.
std::string describeAdapter(ID3D12Device* device)
{
    if (device == nullptr)
        return "no device";

    auto factory = winrt::com_ptr<IDXGIFactory4>();

    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void())))
        return "unknown adapter";

    auto adapter = winrt::com_ptr<IDXGIAdapter1>();

    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),
                                          __uuidof(IDXGIAdapter1),
                                          adapter.put_void())))
        return "unknown adapter";

    auto description = DXGI_ADAPTER_DESC1 {};

    if (FAILED(adapter->GetDesc1(&description)))
        return "unknown adapter";

    return Strings::narrow(description.Description);
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

// Defined in View/GPUView-Windows.cpp: rebuilds every live GPUView's swapchain
// against the recreated device.
void refreshAllGPUViewsForNewDevice();
} // namespace eacp::GPU

namespace eacp::Graphics
{
// Defined in Graphics/D2DFactory-Windows.cpp (linked via eacp-graphics).
void addRenderingDeviceReplacedListener(std::function<void()> listener);
} // namespace eacp::Graphics

namespace eacp::GPU
{
D3D12Shared::D3D12Shared()
{
    createAll();

    // A GPU reset kills the 2D layer's D3D11 device and this D3D12 device
    // together. The graphics layer's recovery fires this listener after it
    // re-established its own device; if ours died too (or never existed),
    // rebuild it and every GPUView swapchain. The 2D layer also replaces its
    // device voluntarily, so a healthy D3D12 device is left alone.
    //
    // Registered here rather than by Device, because there is one of these and
    // there may be any number of Devices â€” a listener each would rebuild the
    // swapchains once per Device.
    Graphics::addRenderingDeviceReplacedListener(
        []
        {
            auto& shared = getD3D12Shared();

            if (shared.isValid()
                && SUCCEEDED(shared.getDevice()->GetDeviceRemovedReason()))
                return;

            shared.recreateAfterDeviceLoss();

            // The shared Device's context is what the swapchains below are
            // rebuilt against, so it is renewed here and now. Every other
            // Device renews on its own thread, at its next acquire().
            getD3D12Context(Device::shared()).renewForNewDevice();
            refreshAllGPUViewsForNewDevice();
        });
}

void D3D12Shared::createAll()
{
    createDevice();

    if (device == nullptr)
        return;

    createRootSignatures();
}

void D3D12Shared::createDevice()
{
    // Before the device, and not merely first: the probe's own device has to
    // be gone before this one is made. See probeDriverQuirks.
    quirks = probeDriverQuirks();
    device = createHardwareOrWarpDevice();
    adapterName = describeAdapter(device.get());
}

void D3D12Shared::createRootSignatures()
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

    // One static sampler per sampling configuration, at register
    // s<configuration> - the register ShaderEmitter points every texture with
    // that sampling at, whatever slot it is in.
    //
    // Samplers are declared here rather than bound per draw from a descriptor
    // heap because a sampler descriptor table cannot be relied on: a
    // Windows-on-Arm driver ignores the table's offset and resolves every sampler
    // to descriptor 0 of the bound heap, so all textures in the process sample
    // through whichever sampler happens to be first. Static samplers never reach
    // a heap and are unaffected. See TextureSampling.
    D3D12_STATIC_SAMPLER_DESC staticSamplers[samplingConfigurations] = {};

    for (auto configuration = 0; configuration < samplingConfigurations;
         ++configuration)
    {
        const auto linear = (configuration & 2) != 0;
        const auto repeat = (configuration & 1) != 0;
        const auto address = repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP
                                    : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

        auto& sampler = staticSamplers[configuration];

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
        sampler.ShaderRegister = static_cast<UINT>(configuration);
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
    // single-descriptor tables. Their registers start above the buffers' â€” the
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
    D3D12_STATIC_SAMPLER_DESC computeSamplers[samplingConfigurations] = {};

    for (auto i = 0; i < samplingConfigurations; ++i)
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

// Built on first use rather than beside the root signatures: a process that
// never dispatches indirectly never makes one, and it depends on nothing that
// could have changed since the device was created.
//
// pRootSignature stays null, which D3D12 allows exactly when every argument is
// a Draw or a Dispatch - nothing about the bindings varies per command here,
// only the grid, so there is no root-argument layout for the signature to
// describe.
ID3D12CommandSignature* D3D12Shared::getDispatchSignature()
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

namespace
{
// The stand-in for a depth resolve, in HLSL: a triangle that covers the target
// from three vertex ids and no vertex buffer, and a pixel shader that reads
// every sample at its pixel and writes the furthest one to SV_Depth - the value
// RESOLVE_MODE_MAX defines, so a target reads the same on both routes.
constexpr auto depthResolveSource = R"(
Texture2DMS<float> source : register(t0);

float4 vertexMain(uint id : SV_VertexID) : SV_Position
{
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float pixelMain(float4 position : SV_Position) : SV_Depth
{
    uint width, height, samples;
    source.GetDimensions(width, height, samples);

    float depth = 0.0;

    for (uint sample = 0; sample < samples; ++sample)
        depth = max(depth, source.Load(int2(position.xy), sample));

    return depth;
}
)";

winrt::com_ptr<ID3DBlob> compileDepthResolveStage(const char* entry,
                                                  const char* target)
{
    auto code = winrt::com_ptr<ID3DBlob>();
    auto errors = winrt::com_ptr<ID3DBlob>();

    if (FAILED(D3DCompile(depthResolveSource,
                          std::strlen(depthResolveSource),
                          nullptr,
                          nullptr,
                          nullptr,
                          entry,
                          target,
                          D3DCOMPILE_ENABLE_STRICTNESS,
                          0,
                          code.put(),
                          errors.put())))
    {
        if (errors != nullptr)
            LOG(static_cast<const char*>(errors->GetBufferPointer()));

        return nullptr;
    }

    return code;
}
} // namespace

const D3D12Shared::DepthResolveShader&
    D3D12Shared::getDepthResolveShader(bool withStencil)
{
    auto& shader = depthResolveShaders[withStencil ? 1 : 0];

    if (shader.isValid() || device == nullptr)
        return shader;

    // One table of one SRV at t0, pixel-visible, and nothing else: the pass
    // binds the multisampled plane and draws.
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;

    const auto parameter = rootTable(&range);

    D3D12_ROOT_SIGNATURE_DESC signatureDesc = {};
    signatureDesc.NumParameters = 1;
    signatureDesc.pParameters = &parameter;

    shader.rootSignature = makeRootSignature(device.get(), signatureDesc);

    const auto vertex = compileDepthResolveStage("vertexMain", "vs_5_0");
    const auto pixel = compileDepthResolveStage("pixelMain", "ps_5_0");

    if (shader.rootSignature == nullptr || vertex == nullptr || pixel == nullptr)
        return shader;

    // Depth only: no colour target, the test always passing and the write
    // always on, so every pixel of the triangle lands its sample.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = shader.rootSignature.get();
    desc.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.DSVFormat = depthAttachmentFormat(withStencil);
    desc.SampleDesc.Count = 1;

    device->CreateGraphicsPipelineState(
        &desc, __uuidof(ID3D12PipelineState), shader.state.put_void());

    return shader;
}

void D3D12Shared::recreateAfterDeviceLoss()
{
    renderRootSignature = nullptr;
    computeRootSignature = nullptr;
    dispatchSignature = nullptr;

    for (auto& shader: depthResolveShaders)
        shader = {};

    device = nullptr;

    ++generation;
    createAll();
}

D3D12Shared& getD3D12Shared()
{
    static auto shared = D3D12Shared();
    return shared;
}

D3D12Context::D3D12Context(D3D12_COMMAND_LIST_TYPE type)
    : queueType(type)
    , owningThreadId(GetCurrentThreadId())
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
    auto& shared = getD3D12Shared();
    generation = shared.getGeneration();

    auto* device = shared.getDevice();

    if (device == nullptr)
        return;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = queueType;

    if (FAILED(device->CreateCommandQueue(
            &queueDesc, __uuidof(ID3D12CommandQueue), queue.put_void()))
        || FAILED(device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), fence.put_void())))
    {
        fence = nullptr;
        queue = nullptr;
        return;
    }

    nextFenceValue = 1;
    lastSubmittedValue = 0;

    textureDescriptors = makeDescriptorAllocator(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, textureHeapCapacity);
    samplerDescriptors = makeDescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                                 samplerHeapCapacity);

    createNullDescriptors();
}

// Tier 1 hardware requires every descriptor table the root signature declares
// to be populated, so the slots a shader does not use still need something
// bound. It has to be something permanently valid: the obvious candidate â€” the
// heap's first descriptor â€” belongs to whichever texture allocated it, and
// descriptor slots are recycled through a free list, so that descriptor can
// come to describe a destroyed resource. Binding it then points the GPU at
// freed memory, which hangs the device rather than failing cleanly.
//
// A null SRV is the case D3D12 provides for exactly this: reads return zero
// and nothing is dereferenced. These two slots are allocated once and never
// freed.
void D3D12Context::createNullDescriptors()
{
    auto* device = getDevice();

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

void D3D12Context::releaseAll()
{
    retired.clear();
    staging.clear();
    readback.clear();
    constantPages.clear();
    recycling.clear();
    reusable.clear();
    reusableBytes = 0;

    // Before the pool it points into goes: a recording open across a device
    // rebuild is one whose list belonged to the dead device either way.
    openRecording = nullptr;
    pool.clear();
    available.clear();
    textureDescriptors = {};
    samplerDescriptors = {};
    nullTexture = {};
    nullTextureUAV = {};
    nullSampler = {};
    fence = nullptr;
    queue = nullptr;
}

void D3D12Context::renewForNewDevice()
{
    releaseAll();
    createAll();
}

void D3D12Context::renewIfDeviceRecreated()
{
    if (generation != getD3D12Shared().getGeneration())
        renewForNewDevice();
}

void D3D12Context::assertOwningThread() const
{
    const auto onOwningThread = mainThreadOwned
                                    ? Threads::isMainThread()
                                    : GetCurrentThreadId() == owningThreadId;

    // Plain ASCII: this string is printed by the CRT to a console that will
    // not be in a codepage that can spell anything else.
    assert(onOwningThread
           && "eacp: a GPU::Device belongs to the thread that made it - give "
              "each thread its own");

    // The assertion is the whole check; a release build pays nothing for it.
    (void) onOwningThread;
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

    auto* device = getDevice();

    if (device == nullptr
        || FAILED(device->CreateDescriptorHeap(
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
        // The heap is shader-visible and cannot be grown in place, so there is
        // nothing to do but say so. Silence here is expensive: the caller gets
        // a slot whose handles are null, writes a view to a null CPU handle and
        // binds a null GPU handle, and what comes back is the device removed
        // with DXGI_ERROR_DRIVER_INTERNAL_ERROR several frames later.
        static auto reported = false;

        if (!reported)
        {
            reported = true;
            LOG("D3D12Context: out of descriptors - the heap holds ",
                allocator.capacity,
                " and every live texture needs one. Textures created from here "
                "on will not be sampleable.");
        }

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

    // Unstamped, because the value that frees it is not knowable yet. Every list
    // that can reference the object from here on is one that already exists: no
    // new command can name it, its owner is gone. But one of those lists may
    // still be open, and an open list's fence value is not assigned until it
    // submits â€” so there is nothing sound to stamp it with until nothing is
    // recording. purgeRetired does that.
    //
    // Stamping it here with the value the *next* signal will carry is what this
    // used to do, and it assumed the open recording submits next. A frame issues
    // uploads of its own, and each used to acquire a second list that signalled
    // *ahead* of the frame's; the frame's list then submitted with a higher value
    // than the stamp, so the stamp completed while the list still referencing the
    // object was open or executing. Releasing there is a use-after-free the debug
    // layer raises on, and it crashed the editor a few keystrokes in.
    retired.add({std::move(object), 0, false});
}

void D3D12Context::purgeRetired()
{
    // Nothing is recording, so every list that could name anything retired so
    // far has been submitted, and lastSubmittedValue is at or past all of their
    // fences. That is the first moment an entry can be given a value that is
    // sound, and it is the whole reason the stamping is here rather than at the
    // point of retirement.
    //
    // Called from the top of acquire(), before the caller's context is taken out
    // of the pool, so the first acquire of a frame sees the previous frame closed
    // and stamps everything it retired.
    //
    // Per context, and only sound because it is: no other context's command list
    // can reference a resource this one retired, since resources do not cross
    // Devices. See getD3D12Context.
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

    // Each goes as its own value passes. Waiting instead for the fence to be
    // past *everything* ever submitted is what this used to do, and under
    // continuous rendering the fence is always a frame or two behind, so it
    // freed nothing at all: a scrolling interface held on to every buffer it had
    // replaced -- thousands of them -- and grew its working set by megabytes a
    // second until it happened to fall idle.
    retired.eraseIf([this](const Retired& entry)
                    { return entry.stamped && hasCompleted(entry.fenceValue); });

    releaseRecycledBuffers();
}

// The same two steps as purgeRetired, for buffers that are to be handed out
// again rather than dropped: stamp them once nothing is recording, and move them
// to the free list once their value passes.
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
    // Best fit rather than first, so a request for a few hundred bytes does not
    // take the megabyte-sized spare and leave the next large one to allocate.
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
    assertOwningThread();
    renewIfDeviceRecreated();

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
        // The allocator and the list are created for the queue type they will
        // execute on: a compute-queue context records COMPUTE lists throughout.
        auto fresh = makeOwned<CommandContext>();
        auto listType = queueType;
        auto* device = getDevice();

        if (device == nullptr
            || FAILED(
                device->CreateCommandAllocator(listType,
                                               __uuidof(ID3D12CommandAllocator),
                                               fresh->allocator.put_void()))
            || FAILED(device->CreateCommandList(0,
                                                listType,
                                                fresh->allocator.get(),
                                                nullptr,
                                                __uuidof(ID3D12GraphicsCommandList),
                                                fresh->list.put_void())))
            return nullptr;

        commands = fresh.get();
        pool.add(std::move(fresh));
    }

    commands->context = this;
    commands->fenceValue = 0;
    commands->recordingId = ++recordingCounter;
    return commands;
}

std::uint64_t D3D12Context::submit(CommandContext* commands)
{
    assertOwningThread();

    if (commands == nullptr || !isValid())
        return 0;

    if (FAILED(commands->list->Close()))
    {
        // An invalid recording (or removed device) must not execute. Nothing
        // reached the GPU, so its staging slots are free at once rather than
        // behind a fence.
        //
        // The context is dropped rather than recycled: a list whose Close()
        // failed cannot be Reset() again on every driver, and one that cannot
        // be is a pooled context every later recording on it silently loses.
        // The next acquire() makes a fresh one.
        reportFailedClose();
        returnStaging(*commands, 0);
        returnConstantPages(*commands, 0);
        pool.removeItem(*commands);
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

// Once per process, so a recording the driver refuses is a line in the log
// rather than a frame that quietly went missing. The removal reason is the
// part worth having: a refused recording and a removed device look the same
// from Close(), and only the device says which it was.
void D3D12Context::reportFailedClose() const
{
    static auto reported = false;

    if (reported)
        return;

    reported = true;

    auto* device = getDevice();
    const auto reason = device != nullptr ? device->GetDeviceRemovedReason() : S_OK;

    char code[16] = {};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(reason));

    LOG("D3D12Context: a recording failed to close; device removed reason ", code);
}

void D3D12Context::discard(CommandContext* commands)
{
    assertOwningThread();

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

CommandContext::UploadChunk* D3D12Context::uploadRoomFor(CommandContext& commands,
                                                         std::size_t bytes)
{
    // Forward only. A chunk the cursor has passed was too full for an earlier
    // request, and going back to check it again on every upload would make this
    // linear in the uploads a frame has already made.
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

    // Left mapped for the resource's whole life. An upload heap is CPU-visible
    // memory, and mapping it per write would be a page-table round trip per
    // draw for a pointer that never changes.
    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};

    if (FAILED(chunk.resource->Map(0, &noRead, &mapped)))
        return nullptr;

    chunk.mapped = static_cast<std::uint8_t*>(mapped);

    return &commands.uploads.add(std::move(chunk));
}

UploadRange D3D12Context::allocateUpload(CommandContext& commands, std::size_t bytes)
{
    // Aligned to the constant requirement whatever the caller wants it for, so
    // that a copy source and a root CBV can share one arena without a copy's
    // odd length pushing the next constant off its 256-byte boundary.
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
    // failing â€” uniform blocks are capped well below it, but nothing here
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
    auto* device = getDevice();

    if (device == nullptr || bytes == 0)
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
} // namespace eacp::GPU
