#include <eacp/Core/Utils/WinInclude.h>

#include "Device.h"

#include "../Windows/D3D12Context.h"

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
struct Device::Native
{
    // Mutable because the accessors that reach it are const — a const Device
    // still submits, the way a const Buffer still reads.
    mutable D3D12Context context;
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
        (instance.impl->context.followMainThread(), true);

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
