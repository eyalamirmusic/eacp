#pragma once

#include "../Linux/GPUBackend-Linux.h"

// The Vulkan half of the Linux seam: one factory per class, each defined in
// that class's own -Vulkan.cpp, so the DeviceBackend can make everything under
// it without any of those files including each other.
namespace eacp::GPU
{
std::unique_ptr<DeviceBackend> makeVulkanDeviceBackend();

// What the auto rule asks of this backend before any Device is made (D8):
// whether there is a Vulkan device here that eacp could use, and whether it is
// a CPU implementation. Asked on an instance of its own, created and destroyed
// inside the call, so a copy that goes on to take the GL backend has not built
// a VkDevice it would never use. EACP_VK_SOFTWARE is not read here - it picks
// among devices once Vulkan is the backend, and the question here is which
// backend.
GPUDeviceClass vulkanDeviceClass();

std::unique_ptr<BufferBackend> makeVulkanBuffer(Device& device,
                                                const void* data,
                                                std::int64_t bytes,
                                                BufferUsage usage,
                                                BufferStorage storage);

std::unique_ptr<TextureBackend> makeVulkanTexture(
    Device& device, const TextureDescriptor& descriptor, const void* pixels);

std::unique_ptr<TextureBackend> wrapVulkanPixelBuffer(Device& device,
                                                      void* nativePixelBuffer);

std::unique_ptr<ShaderLibraryBackend>
    makeVulkanShaderLibrary(Device& device, const ShaderSource& source);

std::unique_ptr<RenderPipelineBackend>
    makeVulkanRenderPipeline(Device& device,
                             const RenderPipelineDescriptor& descriptor);

std::unique_ptr<ComputePipelineBackend>
    makeVulkanComputePipeline(Device& device, const ShaderLibrary& library);

std::unique_ptr<CommandBufferBackend> makeVulkanCommandBuffer(Device& device);

std::unique_ptr<FrameBackend> makeVulkanFrame(Device& device, void* drawable);

std::unique_ptr<FrameBackend> makeVulkanFrame(Device& device,
                                              const OffscreenTarget& target);

std::unique_ptr<GpuTimestampsBackend> makeVulkanGpuTimestamps();

// The two passes are made from an encoder rather than from a Device: whatever
// opened the recording - a Frame or a CommandBuffer - owns that, and the pass
// takes it over.
struct VulkanComputeEncoder;
struct VulkanRenderEncoder;

std::unique_ptr<ComputePassBackend>
    makeVulkanComputePass(VulkanComputeEncoder* encoder, DispatchOrder order);

std::unique_ptr<RenderPassBackend> makeVulkanRenderPass(VulkanRenderEncoder* encoder,
                                                        int targetWidth,
                                                        int targetHeight);

std::unique_ptr<GPUViewBackend> makeVulkanGPUView(GPUView& view,
                                                  Graphics::ViewSurface& record);
} // namespace eacp::GPU
