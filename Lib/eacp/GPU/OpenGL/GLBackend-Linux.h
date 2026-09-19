#pragma once

#include "../Linux/GPUBackend-Linux.h"
#include "GLTypes.h"

// The OpenGL half of the Linux seam, VulkanBackend-Linux.h's twin: one factory
// per class, each defined in that class's own -GL.cpp, so the DeviceBackend can
// make everything under it without any of those files including each other.
namespace eacp::GPU
{
class GLContext;

std::unique_ptr<DeviceBackend> makeGLDeviceBackend();

// The context the Device opened, which is what every object under it renders
// on. Defined beside the DeviceBackend in Device-GL.cpp.
GLContext& getGLContext(const Device& device);

std::unique_ptr<BufferBackend> makeGLBuffer(Device& device,
                                            const void* data,
                                            std::int64_t bytes,
                                            BufferUsage usage,
                                            BufferStorage storage);

std::unique_ptr<TextureBackend> makeGLTexture(Device& device,
                                              const TextureDescriptor& descriptor,
                                              const void* pixels);

std::unique_ptr<TextureBackend> wrapGLPixelBuffer(Device& device,
                                                  void* nativePixelBuffer);

std::unique_ptr<ShaderLibraryBackend>
    makeGLShaderLibrary(Device& device, const ShaderSource& source);

std::unique_ptr<GpuTimestampsBackend> makeGLGpuTimestamps();

std::unique_ptr<RenderPipelineBackend>
    makeGLRenderPipeline(Device& device, const RenderPipelineDescriptor& descriptor);

// D9's refusal until stage 6 builds the tier: a pipeline that is never valid,
// and a Device that answers supportsCompute() false whatever the context has.
std::unique_ptr<ComputePipelineBackend>
    makeGLComputePipeline(Device& device, const ShaderLibrary& library);

std::unique_ptr<CommandBufferBackend> makeGLCommandBuffer(Device& device);

std::unique_ptr<FrameBackend> makeGLFrame(Device& device, void* drawable);

std::unique_ptr<FrameBackend> makeGLFrame(Device& device,
                                          const OffscreenTarget& target);

// The open pass, made by the Frame that will hold it: the encoder is adopted,
// exactly as the Vulkan half hands its own over.
std::unique_ptr<RenderPassBackend> makeGLRenderPass(GLRenderEncoder* encoder);

std::unique_ptr<GPUViewBackend> makeGLGPUView(GPUView& view,
                                              Graphics::ViewSurface& record);

// Where a Vulkan binding number lands in GL's own, much smaller, counts: the
// texture unit, the shader-storage binding point and the uniform-block binding
// point a binding of N is bound at. Defined in RenderPipeline-GL.cpp, beside
// the link-time bind that is their first caller.
int glTextureUnitFor(int binding);
int glStorageBindingPoint(int binding);
int glUniformBindingPoint(int binding);
} // namespace eacp::GPU
