#include <eacp/Core/Utils/WinInclude.h>

#include "Device.h"

#include "../Windows/D3D12Context.h"
#include "../Windows/D3D12Types.h"

#include <algorithm>
#include <memory>
#include <vector>

// Windows/D3D12 backend. A Device owns its D3D12Context — its command queue,
// fence, command-list pool, staging and readback pools, constant ring and
// descriptor heaps — and shares only the ID3D12Device and the root signatures
// with every other Device, through getD3D12Shared(). That is what makes
// `auto worker = GPU::Device();` on a camera thread mean the same thing here as
// it does on Metal, where Device::Native has always held its own MTLCommandQueue.
//
// The 2D graphics layer keeps its own D3D11 device for Direct2D; the compositor
// composes output from both, so the devices never need to be shared.
// nativeQueue() is this Device's command queue.

namespace eacp::GPU
{
namespace
{
D3D12_TEXTURE_ADDRESS_MODE toD3D12AddressMode(SamplerAddressMode mode)
{
    switch (mode)
    {
        case SamplerAddressMode::ClampToEdge:
            return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case SamplerAddressMode::Repeat:
            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case SamplerAddressMode::MirrorRepeat:
            return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case SamplerAddressMode::ClampToBorder:
            return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }

    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

D3D12_COMPARISON_FUNC toD3D12Comparison(CompareFunction compare)
{
    switch (compare)
    {
        case CompareFunction::Never:
            return D3D12_COMPARISON_FUNC_NEVER;
        case CompareFunction::Less:
            return D3D12_COMPARISON_FUNC_LESS;
        case CompareFunction::LessEqual:
            return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareFunction::Equal:
            return D3D12_COMPARISON_FUNC_EQUAL;
        case CompareFunction::NotEqual:
            return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case CompareFunction::GreaterEqual:
            return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case CompareFunction::Greater:
            return D3D12_COMPARISON_FUNC_GREATER;
        case CompareFunction::Always:
            return D3D12_COMPARISON_FUNC_ALWAYS;
    }

    return D3D12_COMPARISON_FUNC_NEVER;
}

D3D12_FILTER_TYPE toD3D12FilterType(SamplerFilter filter)
{
    return filter == SamplerFilter::Linear ? D3D12_FILTER_TYPE_LINEAR
                                           : D3D12_FILTER_TYPE_POINT;
}

D3D12_FILTER toD3D12Filter(const SamplerDescriptor& descriptor)
{
    const auto reduction = descriptor.compare
                               ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
                               : D3D12_FILTER_REDUCTION_TYPE_STANDARD;

    if (descriptor.maxAnisotropy > 1 && descriptor.isFullyLinear())
        return static_cast<D3D12_FILTER>(D3D12_ENCODE_ANISOTROPIC_FILTER(reduction));

    const auto mip = descriptor.mipFilter == SamplerMipFilter::Linear
                         ? D3D12_FILTER_TYPE_LINEAR
                         : D3D12_FILTER_TYPE_POINT;

    return static_cast<D3D12_FILTER>(
        D3D12_ENCODE_BASIC_FILTER(toD3D12FilterType(descriptor.minFilter),
                                  toD3D12FilterType(descriptor.magFilter),
                                  mip,
                                  reduction));
}

void setBorderColor(D3D12_SAMPLER_DESC& desc, SamplerBorderColor color)
{
    const auto white = color == SamplerBorderColor::OpaqueWhite ? 1.f : 0.f;
    const auto alpha = color == SamplerBorderColor::TransparentBlack ? 0.f : 1.f;

    desc.BorderColor[0] = white;
    desc.BorderColor[1] = white;
    desc.BorderColor[2] = white;
    desc.BorderColor[3] = alpha;
}

// D3D12 has no "not mipmapped" filter, so None pins the LOD range to level 0.
D3D12_SAMPLER_DESC toD3D12SamplerDesc(const SamplerDescriptor& descriptor)
{
    const auto mipmapped = descriptor.mipFilter != SamplerMipFilter::None;

    D3D12_SAMPLER_DESC desc = {};
    desc.Filter = toD3D12Filter(descriptor);
    desc.AddressU = toD3D12AddressMode(descriptor.addressU);
    desc.AddressV = toD3D12AddressMode(descriptor.addressV);
    desc.AddressW = toD3D12AddressMode(descriptor.addressW);
    desc.MaxAnisotropy =
        static_cast<UINT>(std::clamp(descriptor.maxAnisotropy, 1, 16));
    desc.ComparisonFunc = descriptor.compare ? toD3D12Comparison(*descriptor.compare)
                                             : D3D12_COMPARISON_FUNC_NEVER;
    desc.MinLOD = mipmapped ? descriptor.lodMinClamp : 0.f;
    desc.MaxLOD = mipmapped ? descriptor.lodMaxClamp : 0.f;
    setBorderColor(desc, descriptor.borderColor);
    return desc;
}
} // namespace

struct Device::Native
{
    // Mutable because the accessors that reach it are const — a const Device
    // still submits, the way a const Buffer still reads.
    mutable D3D12Context context;

    // Each holds a descriptor in this context's sampler heap, which lives as
    // long as the context does; a Sampler handle points at one of these.
    std::vector<std::unique_ptr<D3D12SamplerData>> samplers;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;

    // The process-wide Device is created lazily but belongs to the main thread
    // regardless of which thread asked for it first — every GPUView and every
    // Frame drives it from there. See D3D12Context::followMainThread.
    [[maybe_unused]] static const auto boundToMainThread =
        (instance.followMainThread(),
         instance.impl->context.followMainThread(),
         true);

    return instance;
}

D3D12Context& getD3D12Context(const Device& device)
{
    return *static_cast<D3D12Context*>(device.nativeContext());
}

bool Device::isValid() const
{
    return impl->context.isValid();
}

std::string Device::name() const
{
    if (!isValid())
        return "no D3D12 device";

    return getD3D12Shared().getAdapterName();
}

// D3D12 answers per format rather than per device, so this asks about the two
// formats a multisampled attachment is ever created in here: the colour targets
// eacp makes (BGRA8, the drawable's own) and the combined depth-stencil the
// depth buffer takes. A count both take is a count a pass can be built at.
//
// NumQualityLevels of 0 is the "no" - the call itself succeeds for a count the
// adapter does not offer and reports nothing behind it.
bool Device::supportsSampleCount(int count) const
{
    if (count <= 1)
        return true;

    if (!isValid())
        return false;

    auto* device = getD3D12Shared().getDevice();

    if (device == nullptr)
        return false;

    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM,
                                   DXGI_FORMAT_R8G8B8A8_UNORM,
                                   DXGI_FORMAT_D32_FLOAT_S8X24_UINT};

    for (auto format: formats)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
        levels.Format = format;
        levels.SampleCount = static_cast<UINT>(count);

        if (FAILED(device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)))
            || levels.NumQualityLevels == 0)
            return false;
    }

    return true;
}

// Nothing to ask the device. BC1 through BC5 have been required of every
// feature-level-11 device and BC6H/BC7 of every feature-level-11_0 one, and
// D3D12 has no feature level below 11 at all - so the four formats eacp names
// are there on anything that produced a device in the first place. Metal is
// where this question has an answer that varies, which is why it is a Device
// call rather than a constant.
bool Device::supportsBlockCompression() const
{
    return isValid();
}

// Nothing to ask the device either. A D3D12 shader-resource or unordered-access
// view over a raw buffer starts at an element index, and the element is four
// bytes, so the grid is four on every adapter. Vulkan is where this varies.
int Device::storageBufferOffsetAlignment() const
{
    return 4;
}

bool Device::supportsIndirectCount() const
{
    return isValid();
}

// D3D12_QUERY_TYPE_OCCLUSION counts samples on every device; BINARY_OCCLUSION
// is the AnySamples form.
bool Device::supportsOcclusionQueries() const
{
    return isValid();
}

bool Device::supportsOcclusionSampleCounts() const
{
    return isValid();
}

int Device::maxTextureSlots() const
{
    return isValid() ? GPU::maxTextureSlots : 0;
}

bool Device::supportsVertexTextures() const
{
    return isValid();
}

bool Device::stagesShareTextureSlots() const
{
    return true;
}

int Device::maxComputeBufferSlots() const
{
    return isValid() ? maxBufferSlots : 0;
}

int Device::maxTextureLayers() const
{
    return isValid() ? D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION : 0;
}

int Device::maxTextureSize3D() const
{
    return isValid() ? D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION : 0;
}

bool Device::supportsCubeArrays() const
{
    return isValid();
}

namespace
{
// What the driver reports for a format, past the guarantees: zero where it
// cannot be asked.
UINT formatSupport2(TextureFormat format)
{
    auto* device = getD3D12Shared().getDevice();

    if (device == nullptr)
        return 0;

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
    support.Format = d3d12TextureFormat(format);

    if (FAILED(device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        return 0;

    return static_cast<UINT>(support.Support2);
}

// An sRGB format has no UAV at all on D3D12, nor has R9G9B9E5_SHAREDEXP; the
// caps query would say so, and this says it without asking.
bool canBeComputeWritten(TextureFormat format)
{
    return isRenderableFormat(format) && !isSrgbFormat(format)
           && format != TextureFormat::BGRA8Unorm;
}
} // namespace

// Asked rather than assumed even for the guaranteed formats: a guarantee is not
// a driver, and a kernel whose stores are silently dropped is a great deal
// harder to find than a texture that refused to be created.
bool Device::supportsComputeWrite(TextureFormat format) const
{
    return isValid() && canBeComputeWritten(format)
           && (formatSupport2(format) & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

bool Device::supportsComputeReadWrite(TextureFormat format) const
{
    return supportsComputeWrite(format)
           && (formatSupport2(format) & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
}

// eacp's box-filter kernel reads one level and writes the next through two
// UAVs of the one resource, so the format needs both typed UAV operations.
bool Device::supportsMipGeneration(TextureFormat format) const
{
    return !isIntegerFormat(format) && supportsComputeReadWrite(format);
}

// The D3D11.3 option that lets a vertex shader write
// SV_RenderTargetArrayIndex, rather than a geometry shader eacp does not have.
bool Device::supportsLayeredRendering() const
{
    if (!isValid())
        return false;

    auto* device = getD3D12Shared().getDevice();

    if (device == nullptr)
        return false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};

    if (FAILED(device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        return false;

    const auto fromVertexShader =
        options
            .VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation;

    return fromVertexShader == TRUE;
}

// A shader-model constant rather than an adapter's answer: Direct3D gives a
// thread group 32 KB of groupshared memory at cs_5_0 on every device that runs
// the model at all, so there is nothing to query and nothing that varies.
int Device::maxThreadgroupMemory() const
{
    return isValid() ? 32 * 1024 : 0;
}

// No, and it says nothing about the adapter: eacp compiles HLSL at cs_5_0 under
// FXC, which has no wave matrix operation to lower a fragment to, so one is the
// two-floats-per-lane emulation on every Direct3D device. The packed loads work
// there - each lane unpacks the pair it holds with the same helper a scalar
// packed read uses - and are simply not faster than staging would be.
bool Device::supportsHalfSimdMatrix() const
{
    return false;
}

bool Device::supportsBFloat16SimdMatrix() const
{
    return false;
}

void* Device::nativeContext() const
{
    return &impl->context;
}

void* Device::nativeDevice() const
{
    return impl->context.getDevice();
}

void* Device::nativeQueue() const
{
    return impl->context.getQueue();
}

void* Device::nativeTextureCache() const
{
    // No zero-copy pixel-buffer cache on the D3D12 backend yet; the camera/video
    // path uploads frames through Texture::update instead.
    return nullptr;
}

void* Device::nativeSampler(TextureSampling) const
{
    // D3D12 never binds a sampler: every configuration is a static sampler in
    // the root signature, and the shader picks one by the register it declares
    // its SamplerState on. See TextureSampling.
    return nullptr;
}

// Written into the heap whether or not the root signatures carry sampler
// tables, so a Sampler is valid and cached the same way everywhere; only the
// bind looks at the quirk.
void* Device::createNativeSampler(const SamplerDescriptor& descriptor)
{
    auto& context = impl->context;
    auto slot = context.allocateSamplerDescriptor();

    if (slot.cpu.ptr == 0)
        return nullptr;

    const auto desc = toD3D12SamplerDesc(descriptor);
    context.getDevice()->CreateSampler(&desc, slot.cpu);

    auto data = std::make_unique<D3D12SamplerData>();
    data->slot = slot;

    auto* native = data.get();
    impl->samplers.push_back(std::move(data));
    return native;
}

bool Device::supportsPerBindSamplers() const
{
    return isValid() && hasSamplerTables();
}

void Device::trackSubmittedWork(void*)
{
    // Nothing to record: every submit already stamps this Device's fence, and
    // lastSubmitted() is the value the wait below needs.
}

void Device::waitForSubmittedWork()
{
    impl->context.waitFor(impl->context.lastSubmitted());
}
} // namespace eacp::GPU
