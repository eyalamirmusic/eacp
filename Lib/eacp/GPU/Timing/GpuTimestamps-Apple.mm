#import <Metal/Metal.h>

#include "GpuTimestamps.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Utils/Containers.h>

namespace eacp::GPU
{
namespace
{
// The counter set holding GPUTimestamp. Looked up by name because counterSets
// is a list in no promised order, and a device may carry sets this knows
// nothing about.
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

        // Retained so the slot can be asked whether the GPU has finished with
        // it, and so GPUStartTime/GPUEndTime survive to be read: the buffer is
        // autoreleased, and the pool it came from drains long before a frame
        // three frames later comes looking.
        ObjC::Ptr<NSObject<MTLCommandBuffer>> commandBuffer;
    };

    // Deferred rather than done in the constructor, because this is built by
    // Device and Device::shared() has not returned yet when that runs.
    void ensureCreated()
    {
        if (tried)
            return;

        tried = true;

        auto metal = (__bridge id<MTLDevice>) Device::shared().nativeDevice();

        if (metal == nil)
            return;

        // Stage boundary is the only sampling point Apple silicon offers - not
        // draw, blit, dispatch or tile dispatch - which is the whole reason
        // timing here is per pass rather than a mark anywhere in the frame.
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

    // GPU ticks to milliseconds.
    //
    // sampleTimestamps hands back a simultaneous pair of readings, and both
    // sides of it are nanoseconds - not mach_absolute_time ticks, which on
    // Apple silicon are 41.667ns each and would put every duration out by that
    // factor. The pair is what says what a GPU tick is worth: one pair gives an
    // offset, two give the rate, so this keeps the first from startup and takes
    // another now. By the time anything reads a timing the baseline is seconds
    // long.
    //
    // On Apple silicon the two are the same clock and the ratio comes out at 1.
    // Deriving it anyway costs one call and is what makes this right on a
    // machine where they are not.
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

        // Under a millisecond of baseline the ratio is mostly the cost of the
        // two calls, so take the clocks as one until there is enough of it.
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

    // Unconditional, like endSlot's retain: a device with no counters still
    // times whole frames, so its slots still hold a command buffer.
    //
    // The previous frame's goes now rather than at the next endSlot: it is the
    // only thing the slot holds that is worth anything, and holding it is
    // holding that frame's drawable pool entry too.
    impl->slots[slot].commandBuffer.release();
}

void GpuTimestamps::beginRecording(int, void*)
{
    // Nothing to record. A Metal command buffer times itself, and the whole
    // frame's total comes off GPUStartTime/GPUEndTime once it has run.
}

void* GpuTimestamps::nativeSamples(int slot) const
{
    if (!impl->supported)
        return nullptr;

    return (__bridge void*) impl->slots[slot].samples.get();
}

// The command buffer is retained whether or not the counters exist, because it
// is what carries the frame's own GPU time - and because a slot the timer is
// waiting on cannot be answered without one.
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

    // Error counts as finished: the frame failed, its timestamps are whatever
    // they are, and a slot left pending forever would stop every later frame
    // from being timed at all.
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

                // A sample the GPU never wrote comes back as the error value,
                // and a pass whose stages were dropped can leave end before
                // start. Both mean "no number", not a negative duration.
                const auto missing = start == MTLCounterErrorValue
                                     || end == MTLCounterErrorValue || end < start;

                milliseconds[pass] = missing ? 0.0 : (double) (end - start) * scale;
            }
        }
    }

    // The frame's own total, which needs no counters at all - and so is the one
    // number that still works on a device that cannot sample them.
    const auto elapsed = buffer.GPUEndTime - buffer.GPUStartTime;

    return elapsed > 0.0 ? elapsed * 1000.0 : 0.0;
}
} // namespace eacp::GPU
