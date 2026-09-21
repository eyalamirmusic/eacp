#include "Device.h"

#include "../Vulkan/VulkanBackend-Linux.h"
#include "../Vulkan/VulkanContext.h"

namespace eacp::GPU
{
namespace
{
struct VulkanDeviceBackend final : DeviceBackend
{
    std::string backendName() const override { return "Vulkan"; }

    DeviceBackend* sideFor(GPUApi api) override
    {
        return api == GPUApi::Vulkan ? this : nullptr;
    }

    bool isValid() const override { return context.isValid(); }

    std::string name() const override
    {
        if (!isValid())
            return "no Vulkan device";

        return getVulkanShared().getAdapterName();
    }

    // Every Vulkan device eacp runs on has the compute queue capability the
    // whole kernel tier is built on: the graphics queue family it picks is
    // required to carry VK_QUEUE_COMPUTE_BIT.
    bool supportsCompute() const override { return true; }

    bool supportsStorageBuffers() const override { return true; }

    // Vulkan's clip space is the [0, 1] every eacp projection produces.
    bool supportsZeroToOneDepth() const override { return true; }

    // The intersection of the three masks a pass touches.
    bool supportsSampleCount(int count) const override
    {
        if (count <= 1)
            return true;

        if (!isValid() || count > 64 || (count & (count - 1)) != 0)
            return false;

        const auto& limits = getVulkanShared().getProperties().limits;
        const auto bit = static_cast<VkSampleCountFlags>(count);

        return (limits.framebufferColorSampleCounts & bit) != 0
               && (limits.framebufferDepthSampleCounts & bit) != 0
               && (limits.sampledImageColorSampleCounts & bit) != 0;
    }

    // BC support is optional in Vulkan.
    bool supportsBlockCompression() const override
    {
        return isValid()
               && getVulkanShared().getFeatures().textureCompressionBC == VK_TRUE;
    }

    // The one range rule that is a device property rather than a constant: a
    // descriptor may name no offset off minStorageBufferOffsetAlignment.
    int storageBufferOffsetAlignment() const override
    {
        if (!isValid())
            return 4;

        const auto alignment =
            getVulkanShared().getProperties().limits.minStorageBufferOffsetAlignment;

        return alignment > 0 ? (int) alignment : 4;
    }

    int maxThreadgroupMemory() const override
    {
        if (!isValid())
            return 0;

        return (int) getVulkanShared()
            .getProperties()
            .limits.maxComputeSharedMemorySize;
    }

    // No, and not because of the hardware: eacp emits GLSL 450 with no
    // cooperative-matrix extension, so a fragment here is the two-floats-per-lane
    // emulation whatever the driver could have done. The packed loads work on it -
    // each lane unpacks the pair it holds - and are simply not faster, so there is
    // nothing for a kernel to restructure itself around.
    bool supportsHalfSimdMatrix() const override { return false; }

    bool supportsBFloat16SimdMatrix() const override { return false; }

    void* nativeContext() const override { return &context; }

    void* nativeDevice() const override { return getVulkanShared().getDevice(); }

    void* nativeQueue() const override { return getVulkanShared().getQueue(); }

    void* nativeTextureCache() const override
    {
        // No zero-copy pixel-buffer path on Linux.
        return nullptr;
    }

    // For callers outside the backend: the bind sites take the sampler straight
    // off VulkanShared.
    void* nativeSampler(TextureSampling sampling) const override
    {
        if (!isValid())
            return nullptr;

        return getVulkanShared().getSampler(sampling);
    }

    void followMainThread() override { context.followMainThread(); }

    void waitForSubmittedWork() override { context.waitIdle(); }

    std::unique_ptr<BufferBackend> makeBuffer(Device& device,
                                              const void* data,
                                              std::int64_t bytes,
                                              BufferUsage usage,
                                              BufferStorage storage) override
    {
        return makeVulkanBuffer(device, data, bytes, usage, storage);
    }

    std::unique_ptr<TextureBackend> makeTexture(Device& device,
                                                const TextureDescriptor& descriptor,
                                                const void* pixels) override
    {
        return makeVulkanTexture(device, descriptor, pixels);
    }

    std::unique_ptr<TextureBackend> wrapPixelBuffer(Device& device,
                                                    void* nativePixelBuffer) override
    {
        return wrapVulkanPixelBuffer(device, nativePixelBuffer);
    }

    std::unique_ptr<ShaderLibraryBackend>
        makeShaderLibrary(Device& device, const ShaderSource& source) override
    {
        return makeVulkanShaderLibrary(device, source);
    }

    std::unique_ptr<RenderPipelineBackend>
        makeRenderPipeline(Device& device,
                           const RenderPipelineDescriptor& descriptor) override
    {
        return makeVulkanRenderPipeline(device, descriptor);
    }

    std::unique_ptr<ComputePipelineBackend>
        makeComputePipeline(Device& device, const ShaderLibrary& library) override
    {
        return makeVulkanComputePipeline(device, library);
    }

    std::unique_ptr<CommandBufferBackend> makeCommandBuffer(Device& device) override
    {
        return makeVulkanCommandBuffer(device);
    }

    std::unique_ptr<FrameBackend> makeFrame(Device& device, void* drawable) override
    {
        return makeVulkanFrame(device, drawable);
    }

    std::unique_ptr<FrameBackend> makeFrame(Device& device,
                                            const OffscreenTarget& target) override
    {
        return makeVulkanFrame(device, target);
    }

    std::unique_ptr<GpuTimestampsBackend> makeGpuTimestamps() override
    {
        return makeVulkanGpuTimestamps();
    }

    std::unique_ptr<GPUViewBackend>
        makeGPUView(GPUView& view, Graphics::ViewSurface& record) override
    {
        return makeVulkanGPUView(view, record);
    }

    // Mutable because the accessors that reach it are const, and a const Device
    // still submits.
    mutable VulkanContext context;
};
} // namespace

std::unique_ptr<DeviceBackend> makeVulkanDeviceBackend()
{
    return std::make_unique<VulkanDeviceBackend>();
}

VulkanContext& getVulkanContext(const Device& device)
{
    return *static_cast<VulkanContext*>(
        getDeviceBackend(device, GPUApi::Vulkan).nativeContext());
}
} // namespace eacp::GPU
