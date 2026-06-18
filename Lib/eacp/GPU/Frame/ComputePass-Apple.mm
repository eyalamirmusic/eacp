#import <Metal/Metal.h>

#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct ComputePass::Native
{
    explicit Native(void* encoderHandle)
    {
        if (encoderHandle != nullptr)
            encoder.reset((__bridge NSObject<MTLComputeCommandEncoder>*) encoderHandle);
    }

    ObjC::Ptr<NSObject<MTLComputeCommandEncoder>> encoder;
    bool ended = false;
};

ComputePass::ComputePass(void* encoder)
    : impl(encoder)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    auto activeEncoder = impl->encoder.get();
    auto state = (__bridge id<MTLComputePipelineState>) pipeline.nativeState();

    if (activeEncoder != nil && state != nil)
        [activeEncoder setComputePipelineState:state];
}

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    auto activeEncoder = impl->encoder.get();
    auto metalBuffer = (__bridge id<MTLBuffer>) buffer.nativeBuffer();

    if (activeEncoder != nil && metalBuffer != nil)
        [activeEncoder setBuffer:metalBuffer offset:0 atIndex:(NSUInteger) slot];
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    // Metal binds a device buffer the same way whether the kernel reads or
    // writes it; the read/write distinction only matters to D3D's view types.
    setInputBuffer(buffer, slot);
}

void ComputePass::setBytes(const void* data, std::size_t bytes, int slot)
{
    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder setBytes:data
                         length:bytes
                        atIndex:(NSUInteger) (uniformBase + slot)];
}

void ComputePass::dispatch(int count)
{
    auto activeEncoder = impl->encoder.get();

    if (activeEncoder == nil || count <= 0)
        return;

    auto width = (NSUInteger) threadGroupWidth;
    auto groups = ((NSUInteger) count + width - 1) / width;

    [activeEncoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                  threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
}

void ComputePass::end()
{
    if (impl->ended)
        return;

    if (auto activeEncoder = impl->encoder.get())
        [activeEncoder endEncoding];

    impl->ended = true;
}
} // namespace eacp::GPU
