#import <Metal/Metal.h>

#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct ComputePipeline::Native
{
    Native(Device& device, const ShaderLibrary& library)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();
        auto metalLibrary = (__bridge id<MTLLibrary>) library.nativeLibrary();

        if (metalDevice == nil || metalLibrary == nil)
            return;

        auto kernelName = @(library.computeEntry().c_str());
        id<MTLFunction> kernel = [metalLibrary newFunctionWithName:kernelName];

        if (kernel == nil)
            return;

        NSError* error = nil;
        state = [metalDevice newComputePipelineStateWithFunction:kernel error:&error];

        if (state.get() == nil && error != nil)
            LOG(error.localizedDescription.UTF8String);

        [kernel release];
    }

    ObjC::Ptr<NSObject<MTLComputePipelineState>> state;
};

ComputePipeline::ComputePipeline(Device& device, const ShaderLibrary& library)
    : impl(device, library)
{
}

bool ComputePipeline::isValid() const
{
    return impl->state.get() != nil;
}

void* ComputePipeline::nativeState() const
{
    return (__bridge void*) impl->state.get();
}
} // namespace eacp::GPU
