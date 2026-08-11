#import <Metal/Metal.h>

#include "GpuTimestamps.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Utils/Containers.h>

namespace eacp::GPU
{
namespace
{
// By name: counterSets is in no promised order.
id<MTLCounterSet> findTimestampSet(id<MTLDevice> device)
{
    for (id<MTLCounterSet> set in device.counterSets)
        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
            return set;

    return nil;
}

} // namespace

struct GpuTimestamps::Native
{
    struct Slot
    {
        ObjC::Ptr<NSObject<MTLCounterSampleBuffer>> samples;

        // Retained: the buffer is autoreleased and its pool drains long before
        // the slot is read.
        ObjC::Ptr<NSObject<MTLCommandBuffer>> commandBuffer;
    };

    // Deferred: Device builds this, and Device::shared() has not returned yet.
    void ensureCreated()
    {
        if (tried)
            return;

        tried = true;

        auto metal = (__bridge id<MTLDevice>) Device::shared().nativeDevice();

        if (metal == nil)
            return;

        // Stage boundary is Apple silicon's only sampling point, which is why
        // timing is per pass rather than at any mark in the frame.
        if (![metal supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
            return;

        auto set = findTimestampSet(metal);

        if (set == nil)
            return;

        auto descriptor = ObjC::makePtr<MTLCounterSampleBufferDescriptor>();
        descriptor.get().counterSet = set;
        descriptor.get().storageMode = MTLStorageModeShared;
        descriptor.get().sampleCount = (NSUInteger) (maxTimedPasses * 2);

        for (auto& slot : slots)
        {
            NSError* error = nil;
            auto buffer = [metal newCounterSampleBufferWithDescriptor:descriptor.get()
                                                                error:&error];

            if (buffer == nil)
                return;

            slot.samples.set((NSObject<MTLCounterSampleBuffer>*) buffer);
        }

        [metal sampleTimestamps:&firstCpu gpuTimestamp:&firstGpu];

        supported = true;
    }

    // Both sides of sampleTimestamps' pair are nanoseconds, not
    // mach_absolute_time ticks. Two pairs give the CPU/GPU rate: one kept from
    // startup, one taken now. The ratio is 1 on Apple silicon.
    double ticksToMilliseconds() const
    {
        auto metal = (__bridge id<MTLDevice>) Device::shared().nativeDevice();

        if (metal == nil)
            return 0.0;

        MTLTimestamp cpu = 0;
        MTLTimestamp gpu = 0;
        [metal sampleTimestamps:&cpu gpuTimestamp:&gpu];

        const auto cpuElapsed = (double) (cpu - firstCpu);
        const auto gpuElapsed = (double) (gpu - firstGpu);

        // Under a millisecond of baseline the ratio is mostly call overhead.
        const auto scale = gpuElapsed > 0.0 && cpuElapsed > 1e6
                               ? cpuElapsed / gpuElapsed
                               : 1.0;

        return scale * 1e-6;
    }

    Array<Slot, slotCount> slots;

    MTLTimestamp firstCpu = 0;
    MTLTimestamp firstGpu = 0;

    bool supported = false;
    bool tried = false;
};

GpuTimestamps::GpuTimestamps() = default;
GpuTimestamps::~GpuTimestamps() = default;

bool GpuTimestamps::isSupported() const
{
    return impl->supported;
}

void GpuTimestamps::beginSlot(int slot)
{
    impl->ensureCreated();

    // Released here rather than at the next endSlot: holding the command buffer
    // holds that frame's drawable pool entry too.
    impl->slots[slot].commandBuffer.release();
}

void GpuTimestamps::beginRecording(int, void*)
{
    // A Metal command buffer times itself, via GPUStartTime/GPUEndTime.
}

void* GpuTimestamps::nativeSamples(int slot) const
{
    if (!impl->supported)
        return nullptr;

    return (__bridge void*) impl->slots[slot].samples.get();
}

// Retained whether or not counters exist: it carries the frame's own GPU time.
bool GpuTimestamps::endSlot(int slot, int, void* nativeCommandBuffer)
{
    impl->slots[slot].commandBuffer.reset(
        (__bridge NSObject<MTLCommandBuffer>*) nativeCommandBuffer);

    return impl->slots[slot].commandBuffer.get() != nil;
}

void GpuTimestamps::noteSubmitted(int, std::uint64_t)
{
    // A D3D12 idea. Metal's command buffer knows its own status.
}

bool GpuTimestamps::isSlotComplete(int slot) const
{
    auto buffer = (id<MTLCommandBuffer>) impl->slots[slot].commandBuffer.get();

    if (buffer == nil)
        return false;

    const auto status = buffer.status;

    // Error counts as finished, or the slot stays pending forever and stops
    // every later frame from being timed.
    return status == MTLCommandBufferStatusCompleted
           || status == MTLCommandBufferStatusError;
}

double GpuTimestamps::resolveSlot(int slot, int passCount, double* milliseconds)
{
    auto& entry = impl->slots[slot];
    auto buffer = (id<MTLCommandBuffer>) entry.commandBuffer.get();
    auto samples = (id<MTLCounterSampleBuffer>) entry.samples.get();

    if (buffer == nil)
        return 0.0;

    if (samples != nil && passCount > 0)
    {
        const auto count = (NSUInteger) (passCount * 2);
        auto data = [samples resolveCounterRange:NSMakeRange(0, count)];

        if (data != nil && data.length >= count * sizeof(MTLCounterResultTimestamp))
        {
            const auto* results = (const MTLCounterResultTimestamp*) data.bytes;
            const auto scale = impl->ticksToMilliseconds();

            for (auto pass = 0; pass < passCount; ++pass)
            {
                const auto start = results[pass * 2].timestamp;
                const auto end = results[pass * 2 + 1].timestamp;

                // An unwritten sample reads as the error value, and dropped
                // stages can leave end before start. Both mean "no number".
                const auto missing = start == MTLCounterErrorValue
                                     || end == MTLCounterErrorValue || end < start;

                milliseconds[pass] = missing ? 0.0 : (double) (end - start) * scale;
            }
        }
    }

    // Needs no counters, so it still works where sampling is unsupported.
    const auto elapsed = buffer.GPUEndTime - buffer.GPUStartTime;

    return elapsed > 0.0 ? elapsed * 1000.0 : 0.0;
}
} // namespace eacp::GPU
