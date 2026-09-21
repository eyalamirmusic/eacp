#include "Device.h"

#include "../Linux/LinuxGPUBackend-Linux.h"

#include <eacp/Core/Utils/Environment.h>

namespace eacp::GPU
{
struct Device::Native
{
    Native()
        : backend(makeDeviceBackend())
    {
    }

    std::unique_ptr<DeviceBackend> backend;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;

    // Belongs to the main thread whichever thread asked for it first.
    [[maybe_unused]] static const auto boundToMainThread =
        (instance.followMainThread(),
         instance.impl->backend->followMainThread(),
         true);

    return instance;
}

DeviceBackend& getDeviceBackend(const Device& device)
{
    return *static_cast<DeviceBackend*>(device.nativeContext());
}

DeviceBackend& getDeviceBackend(const Device& device, GPUApi api)
{
    return *getDeviceBackend(device).sideFor(api);
}

std::string Device::backendName() const
{
    return impl->backend->backendName();
}

bool Device::isValid() const
{
    return impl->backend->isValid();
}

std::string Device::name() const
{
    return impl->backend->name();
}

// EACP_GPU_NO_COMPUTE=1 takes the answer away on a device that has it, so the
// routes an interface takes without a compute tier stay reachable in a test on
// a device that would otherwise never take them.
bool Device::supportsCompute() const
{
    if (getEnvValue("EACP_GPU_NO_COMPUTE") == "1")
        return false;

    return impl->backend->supportsCompute();
}

bool Device::supportsStorageBuffers() const
{
    return impl->backend->supportsStorageBuffers();
}

bool Device::supportsZeroToOneDepth() const
{
    return impl->backend->supportsZeroToOneDepth();
}

bool Device::supportsSampleCount(int count) const
{
    return impl->backend->supportsSampleCount(count);
}

bool Device::supportsBlockCompression() const
{
    return impl->backend->supportsBlockCompression();
}

int Device::storageBufferOffsetAlignment() const
{
    return impl->backend->storageBufferOffsetAlignment();
}

int Device::maxThreadgroupMemory() const
{
    return impl->backend->maxThreadgroupMemory();
}

bool Device::supportsHalfSimdMatrix() const
{
    return impl->backend->supportsHalfSimdMatrix();
}

bool Device::supportsBFloat16SimdMatrix() const
{
    return impl->backend->supportsBFloat16SimdMatrix();
}

// The backend itself, which is what getDeviceBackend hands every object this
// Device makes. Its own per-Device state is one virtual call further in.
void* Device::nativeContext() const
{
    return impl->backend.get();
}

void* Device::nativeDevice() const
{
    return impl->backend->nativeDevice();
}

void* Device::nativeQueue() const
{
    return impl->backend->nativeQueue();
}

void* Device::nativeTextureCache() const
{
    return impl->backend->nativeTextureCache();
}

void* Device::nativeSampler(TextureSampling sampling) const
{
    return impl->backend->nativeSampler(sampling);
}

void Device::trackSubmittedWork(void*)
{
    // Nothing to record: every submit already signals this Device's timeline.
}

void Device::waitForSubmittedWork()
{
    impl->backend->waitForSubmittedWork();
}
} // namespace eacp::GPU
