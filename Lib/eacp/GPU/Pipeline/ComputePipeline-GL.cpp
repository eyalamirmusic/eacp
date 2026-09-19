#include "ComputePipeline.h"

#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"

#include <eacp/Core/Utils/Logging.h>

// The GL compute tier is stage 6's (plan.md D9). Until it is built, a kernel
// has nowhere to run here: the pipeline is never valid, Frame::beginCompute and
// CommandBuffer::beginCompute answer null, and Device::supportsCompute() is
// false whatever the context's own compute stage says - which is what sends the
// UI down the mesh route (D10) and what every kernel test self-skips on.
namespace eacp::GPU
{
namespace
{
struct GLUnbuiltComputePipeline final : ComputePipelineBackend
{
    GLUnbuiltComputePipeline()
    {
        static auto said = false;

        if (std::exchange(said, true))
            return;

        LOG("OpenGL: the compute tier is not built on this backend yet, so no "
            "kernel runs here - Device::supportsCompute() says so");
    }

    bool isValid() const override { return false; }

    int threadExecutionWidth() const override { return 0; }

    void* nativeState() const override { return nullptr; }
};
} // namespace

std::unique_ptr<ComputePipelineBackend> makeGLComputePipeline(Device&,
                                                              const ShaderLibrary&)
{
    return std::make_unique<GLUnbuiltComputePipeline>();
}
} // namespace eacp::GPU
