#include "Device.h"

#include "../WebGPU/WebGPUTypes.h"

namespace eacp::GPU
{
struct Device::Native
{
    // Mutable because the accessors that reach it are const, and a const Device
    // still submits.
    mutable WebGPUContext context;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;

    [[maybe_unused]] static const auto boundToMainThread =
        (instance.followMainThread(), true);

    return instance;
}

bool Device::isValid() const
{
    return impl->context.isValid();
}

std::string Device::name() const
{
    return getWebGPUShared().getAdapterName();
}

// WebGPU offers one and four and nothing else.
bool Device::supportsSampleCount(int count) const
{
    if (count <= 1)
        return true;

    return isValid() && count == 4;
}

bool Device::supportsBlockCompression() const
{
    return isValid()
           && getWebGPUShared().hasFeature(WGPUFeatureName_TextureCompressionBC);
}

int Device::storageBufferOffsetAlignment() const
{
    if (!isValid())
        return 4;

    const auto alignment =
        getWebGPUShared().getLimits().minStorageBufferOffsetAlignment;

    return alignment > 0 ? (int) alignment : 4;
}

int Device::maxThreadgroupMemory() const
{
    if (!isValid())
        return 0;

    return (int) getWebGPUShared().getLimits().maxComputeWorkgroupStorageSize;
}

// WGSL has no SIMD-group matrix type, so a fragment is the per-lane emulation
// the other non-Metal backends run.
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
    return getWebGPUShared().getDevice();
}

void* Device::nativeQueue() const
{
    return getWebGPUShared().getQueue();
}

void* Device::nativeTextureCache() const
{
    return nullptr;
}

void* Device::nativeSampler(TextureSampling sampling) const
{
    if (!isValid())
        return nullptr;

    return getWebGPUShared().getSampler(sampling, true);
}

void Device::trackSubmittedWork(void*) {}

// Waiting would block the browser's main thread, which never gives the GPU a
// chance to finish: see the Web section of GPU/README.md.
void Device::waitForSubmittedWork()
{
    if (!impl->context.hasCompleted(impl->context.lastSubmitted()))
        reportWebUnsupported("Device::waitForSubmittedWork");
}
} // namespace eacp::GPU
