#include "GpuTimestamps.h"

#include "../Device/Device.h"
#include "../Linux/GPUBackend-Linux.h"

namespace eacp::GPU
{
// The backend arrives with the first slot rather than with the object: this is
// built by a Device, which is not yet itself when that runs, and beginSlot is
// the first call that carries one. Everything before it answers as an
// unsupported timer, which is what a Device with no timestamp counters answers
// for the whole of its life.
struct GpuTimestamps::Native
{
    GpuTimestampsBackend* forDevice(Device& device)
    {
        if (backend == nullptr)
            backend = getDeviceBackend(device).makeGpuTimestamps();

        return backend.get();
    }

    std::unique_ptr<GpuTimestampsBackend> backend;
};

GpuTimestamps::GpuTimestamps() = default;
GpuTimestamps::~GpuTimestamps() = default;

bool GpuTimestamps::isSupported() const
{
    return impl->backend != nullptr && impl->backend->isSupported();
}

void GpuTimestamps::beginSlot(int slot, Device& device)
{
    impl->forDevice(device)->beginSlot(slot, device);
}

void GpuTimestamps::beginRecording(int slot, void* nativeCommandBuffer)
{
    if (impl->backend != nullptr)
        impl->backend->beginRecording(slot, nativeCommandBuffer);
}

void* GpuTimestamps::nativeSamples(int slot) const
{
    return impl->backend != nullptr ? impl->backend->nativeSamples(slot) : nullptr;
}

bool GpuTimestamps::endSlot(int slot, int passCount, void* nativeCommandBuffer)
{
    return impl->backend != nullptr
           && impl->backend->endSlot(slot, passCount, nativeCommandBuffer);
}

void GpuTimestamps::noteSubmitted(int slot, std::uint64_t fenceValue)
{
    if (impl->backend != nullptr)
        impl->backend->noteSubmitted(slot, fenceValue);
}

bool GpuTimestamps::isSlotComplete(int slot, const Device& device) const
{
    return impl->backend != nullptr && impl->backend->isSlotComplete(slot, device);
}

double GpuTimestamps::resolveSlot(int slot, int passCount, double* milliseconds)
{
    if (impl->backend == nullptr)
        return 0.0;

    return impl->backend->resolveSlot(slot, passCount, milliseconds);
}
} // namespace eacp::GPU
