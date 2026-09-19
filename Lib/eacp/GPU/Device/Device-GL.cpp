#include "Device.h"

#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"

namespace eacp::GPU
{
namespace
{
GLenum glMagFilterFor(const TextureSampling& sampling)
{
    return sampling.filter == TextureFilter::Linear ? GL_LINEAR : GL_NEAREST;
}

// The mip filter goes with the magnification one, as VkSamplerCreateInfo's
// mipmapMode does beside its two: a texture with one level is complete under
// either, its max level having been fixed at creation.
GLenum glMinFilterFor(const TextureSampling& sampling)
{
    return sampling.filter == TextureFilter::Linear ? GL_LINEAR_MIPMAP_LINEAR
                                                    : GL_NEAREST_MIPMAP_NEAREST;
}

GLenum glWrapFor(const TextureSampling& sampling)
{
    return sampling.addressMode == TextureAddressMode::Repeat ? GL_REPEAT
                                                              : GL_CLAMP_TO_EDGE;
}

struct GLDeviceBackend final : DeviceBackend
{
    std::string backendName() const override { return "OpenGL"; }

    bool isValid() const override { return context.isValid(); }

    std::string name() const override
    {
        if (!isValid())
            return "no OpenGL device";

        return context.getCapabilities().renderer;
    }

    // False on every context until stage 6 builds the tier, whatever compute
    // stage the context itself has: a kernel has nowhere to run here, so the
    // UI takes the mesh route (D10) and every kernel test self-skips on this
    // rather than on a pipeline that would never be valid (D9, D12).
    bool supportsCompute() const override { return false; }

    bool supportsSampleCount(int count) const override
    {
        if (count <= 1)
            return true;

        if (!isValid() || (count & (count - 1)) != 0)
            return false;

        return count <= context.getCapabilities().maxSamples;
    }

    bool supportsBlockCompression() const override
    {
        if (!isValid())
            return false;

        // Both families, since a texture in either is refused at creation where
        // its own extension is missing - the same question the Vulkan backend
        // answers with one feature bit.
        const auto& caps = context.getCapabilities();

        return caps.s3tc && caps.bptc;
    }

    // One number for both binds, which is the larger of the two grids
    // glBindBufferRange enforces: a caller that rounds its offsets to it
    // satisfies the uniform ring and a storage range alike, and a second query
    // on Device would only be the smaller of the two under another name.
    int storageBufferOffsetAlignment() const override
    {
        if (!isValid())
            return 4;

        const auto& caps = context.getCapabilities();
        const auto alignment = caps.uniformBufferOffsetAlignment
                                       > caps.storageBufferOffsetAlignment
                                   ? caps.uniformBufferOffsetAlignment
                                   : caps.storageBufferOffsetAlignment;

        return alignment > 0 ? alignment : 4;
    }

    int maxThreadgroupMemory() const override
    {
        return isValid() ? context.getCapabilities().maxThreadgroupMemory : 0;
    }

    // No cooperative-matrix anything at any GL version, so a fragment is the
    // two-floats-per-lane emulation whatever the hardware could have done -
    // the Vulkan answer, for the same reason.
    bool supportsHalfSimdMatrix() const override { return false; }

    bool supportsBFloat16SimdMatrix() const override { return false; }

    void* nativeContext() const override { return &context; }

    void* nativeDevice() const override { return context.getContext(); }

    // No queue: a GL context is its own stream, and what would be submitted to
    // one is already recorded by the call that made it.
    void* nativeQueue() const override { return nullptr; }

    void* nativeTextureCache() const override { return nullptr; }

    // The sampler object itself rather than a pointer to it, as the Vulkan
    // backend hands back its VkSampler: a GL name of 0 is never a made one, so
    // null and invalid still mean the same thing.
    void* nativeSampler(TextureSampling sampling) const override
    {
        if (!isValid())
            return nullptr;

        context.makeCurrent();

        auto& sampler = samplers[samplingIndex(sampling)];

        if (sampler == 0)
        {
            glGenSamplers(1, &sampler);

            const auto wrap = glWrapFor(sampling);

            glSamplerParameteri(
                sampler, GL_TEXTURE_MIN_FILTER, (GLint) glMinFilterFor(sampling));
            glSamplerParameteri(
                sampler, GL_TEXTURE_MAG_FILTER, (GLint) glMagFilterFor(sampling));
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, (GLint) wrap);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, (GLint) wrap);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, (GLint) wrap);
        }

        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(sampler));
    }

    // The context is current on whichever thread built the Device, and the
    // shared one is driven by the main thread afterwards: giving it up here is
    // what lets that thread take it, an EGLContext being current on one thread
    // at a time.
    void followMainThread() override { context.releaseCurrent(); }

    void waitForSubmittedWork() override
    {
        if (!isValid())
            return;

        context.makeCurrent();
        glFinish();
    }

    std::unique_ptr<BufferBackend> makeBuffer(Device& device,
                                              const void* data,
                                              std::int64_t bytes,
                                              BufferUsage usage,
                                              BufferStorage storage) override
    {
        return makeGLBuffer(device, data, bytes, usage, storage);
    }

    std::unique_ptr<TextureBackend> makeTexture(Device& device,
                                                const TextureDescriptor& descriptor,
                                                const void* pixels) override
    {
        return makeGLTexture(device, descriptor, pixels);
    }

    std::unique_ptr<TextureBackend> wrapPixelBuffer(Device& device,
                                                    void* nativePixelBuffer) override
    {
        return wrapGLPixelBuffer(device, nativePixelBuffer);
    }

    std::unique_ptr<ShaderLibraryBackend>
        makeShaderLibrary(Device& device, const ShaderSource& source) override
    {
        return makeGLShaderLibrary(device, source);
    }

    std::unique_ptr<RenderPipelineBackend>
        makeRenderPipeline(Device& device,
                           const RenderPipelineDescriptor& descriptor) override
    {
        return makeGLRenderPipeline(device, descriptor);
    }

    std::unique_ptr<ComputePipelineBackend>
        makeComputePipeline(Device& device, const ShaderLibrary& library) override
    {
        return makeGLComputePipeline(device, library);
    }

    std::unique_ptr<CommandBufferBackend> makeCommandBuffer(Device& device) override
    {
        return makeGLCommandBuffer(device);
    }

    std::unique_ptr<FrameBackend> makeFrame(Device& device, void* drawable) override
    {
        return makeGLFrame(device, drawable);
    }

    std::unique_ptr<FrameBackend> makeFrame(Device& device,
                                            const OffscreenTarget& target) override
    {
        return makeGLFrame(device, target);
    }

    std::unique_ptr<GpuTimestampsBackend> makeGpuTimestamps() override
    {
        return makeGLGpuTimestamps();
    }

    std::unique_ptr<GPUViewBackend>
        makeGPUView(GPUView& view, Graphics::ViewSurface& record) override
    {
        return makeGLGPUView(view, record);
    }

    ~GLDeviceBackend() override
    {
        if (!context.isValid())
            return;

        context.makeCurrent();

        for (auto& sampler: samplers)
            if (sampler != 0)
                glDeleteSamplers(1, &sampler);
    }

    // Mutable because the accessors that reach them are const, and a const
    // Device still draws.
    mutable GLContext context;
    mutable Array<GLuint, samplingConfigurations> samplers {};
};
} // namespace

std::unique_ptr<DeviceBackend> makeGLDeviceBackend()
{
    return std::make_unique<GLDeviceBackend>();
}

GLContext& getGLContext(const Device& device)
{
    return *static_cast<GLContext*>(getDeviceBackend(device).nativeContext());
}
} // namespace eacp::GPU
