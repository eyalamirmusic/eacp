#import <Metal/Metal.h>

#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../Shader/ShaderLibrary.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <cassert>

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
        checkGroupFits(library.threadGroupShape());
    }

    // A group larger than the pipeline allows dispatches nothing at all, so it
    // is reported here rather than left as an empty output.
    void checkGroupFits(ThreadGroupShape shape)
    {
        if (state.get() == nil || !shape.isSet())
            return;

        const auto allowed = (int) state.get().maxTotalThreadsPerThreadgroup;

        if (shape.threadCount() <= allowed)
            return;

        LOG("eacp: a threadgroup of ",
            shape.x,
            "x",
            shape.y,
            "x",
            shape.z,
            " is ",
            shape.threadCount(),
            " threads, and this pipeline allows at most ",
            allowed);

        assert(false && "eacp: the kernel's threadgroup is larger than this "
                        "device dispatches");
    }

    ObjC::Ptr<NSObject<MTLComputePipelineState>> state;
};

ComputePipeline::ComputePipeline(Device& device, const ShaderLibrary& library)
    : groupShape(library.threadGroupShape())
    , impl(device, library)
{
}

bool ComputePipeline::isValid() const
{
    return impl->state.get() != nil;
}

int ComputePipeline::threadExecutionWidth() const
{
    if (impl->state.get() == nil)
        return 0;

    return (int) impl->state.get().threadExecutionWidth;
}

void* ComputePipeline::nativeState() const
{
    return (__bridge void*) impl->state.get();
}
} // namespace eacp::GPU
