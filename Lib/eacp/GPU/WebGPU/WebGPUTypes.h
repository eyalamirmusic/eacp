#pragma once

#include "../Codegen/WgslBindings.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Shader/ShaderSource.h"
#include "WebGPUContext.h"

// The records the backend's translation units hand each other through the
// opaque native handles. Not part of GPU.h.

namespace eacp::GPU
{
// The bind group numbering is the WGSL emitter's, in Codegen/WgslBindings.h.
constexpr int webMaxBufferSlots = ComputePass::maxBufferSlots;

static_assert(wgslBufferBinding(webMaxBufferSlots - 1) < wgslSamplerBase,
              "every storage buffer binding must stay below the samplers");

enum class WebBindingKind
{
    Uniform,
    Texture,
    TextureCube,
    DepthTexture,
    StorageTexture,
    Sampler,
    StorageBuffer,
    ReadOnlyStorageBuffer
};

struct WebBinding
{
    int binding = 0;
    WebBindingKind kind = WebBindingKind::Texture;

    // The texture, buffer or sampler slot the binding number stands for.
    int slot = 0;

    // Textures and samplers: whether the slot filters. Nearest slots are
    // unfilterable-float with a non-filtering sampler, which any float format
    // takes - r32float included, on a device without float32-filterable.
    bool filtering = true;

    WGPUTextureFormat storageFormat = WGPUTextureFormat_Undefined;
};

// What a shader's one bind group holds, reflected from its WGSL declarations
// the way the Vulkan backend reflects SPIR-V, plus the sampling each texture
// slot was declared with, which WGSL does not carry.
struct WebBindingLayout
{
    const WebBinding* find(WebBindingKind kind, int slot) const;
    bool hasUniforms() const;

    Vector<WebBinding> entries;
    bool compute = false;
};

WebBindingLayout reflectWgslBindings(const std::string& wgsl,
                                     bool compute,
                                     const Vector<ResourceBinding>& declared);

// Null when the layout breaks a device limit, which is logged.
WGPUBindGroupLayout makeBindGroupLayout(const WebBindingLayout& layout,
                                        std::string_view label);

struct WebShaderProgram
{
    bool isValid() const { return module != nullptr && pipelineLayout != nullptr; }

    WGPUShaderModule module = nullptr;
    WGPUBindGroupLayout groupLayout = nullptr;
    WGPUPipelineLayout pipelineLayout = nullptr;
    WebBindingLayout bindings;
};

struct WebBufferData
{
    WGPUBuffer buffer = nullptr;

    // What the caller asked for; the allocation is this rounded up to four.
    std::size_t size = 0;
    std::size_t allocated = 0;

    // A Streaming buffer keeps what the CPU last wrote, so read() answers
    // without a round trip, as it does on the backends that map one.
    Vector<std::byte> shadow;
    bool streaming = false;
};

// The storage-buffer range a bind names: from the offset to the buffer's end.
struct WebBufferBind
{
    bool isValid() const { return buffer != nullptr; }

    WGPUBuffer buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

WebBufferBind webStorageBufferBind(const BufferRange& range);

struct WebTextureData
{
    bool isValid() const
    {
        return attachmentView != nullptr || sampledView != nullptr;
    }
    bool isRenderTarget() const { return attachmentView != nullptr; }
    bool isComputeWritable() const { return storageView != nullptr; }
    bool isMultisampled() const { return msaaView != nullptr; }
    bool hasDepth() const { return depthView != nullptr; }
    bool hasStencil() const { return hasDepth() && depthHasStencil; }
    bool hasSampleableDepth() const { return depthReadView != nullptr; }

    // The view a pass renders into: the multisample one where there is one,
    // resolved into attachmentView.
    WGPUTextureView colorAttachmentView() const
    {
        return isMultisampled() ? msaaView : attachmentView;
    }

    WGPUTexture texture = nullptr;
    WGPUTextureView sampledView = nullptr;
    WGPUTextureView attachmentView = nullptr;
    WGPUTextureView storageView = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;

    int width = 0;
    int height = 0;
    int mipLevels = 1;
    int sampleCount = 1;
    bool cube = false;

    WGPUTexture msaaTexture = nullptr;
    WGPUTextureView msaaView = nullptr;

    WGPUTexture depthTexture = nullptr;
    WGPUTextureView depthView = nullptr;
    WGPUTextureView depthReadView = nullptr;
    WGPUTextureFormat depthFormat = WGPUTextureFormat_Undefined;
    bool depthHasStencil = false;
};

WGPUTextureFormat toWebFormat(TextureFormat format);
WGPUTextureFormat toWebFormat(PixelFormat format);
WGPUTextureFormat webDepthFormat(bool stencil);
bool webFormatHasStencil(WGPUTextureFormat format);

// Each false leaves `data` without the companion and says why.
bool createWebMultisampleCompanion(WebTextureData& data);
bool createWebDepthCompanion(WebTextureData& data, bool stencil, bool sampleable);
void releaseWebCompanions(WebTextureData& data);
void releaseWebTexture(WebTextureData& data);

struct WebRenderPipeline
{
    WebRenderPipeline() = default;
    ~WebRenderPipeline();

    WebRenderPipeline(const WebRenderPipeline&) = delete;
    WebRenderPipeline& operator=(const WebRenderPipeline&) = delete;

    bool isValid() const { return pipeline != nullptr; }

    // The pipeline as built, or one of its variants for a pass whose depth
    // attachment differs from the one it was described against: WebGPU refuses
    // the mismatch Metal and Vulkan let through, and a HUD without a depth test
    // drawn into a depth-tested pass is the ordinary case.
    WGPURenderPipeline forDepthFormat(WGPUTextureFormat passDepth);

    WGPURenderPipeline pipeline = nullptr;
    WGPUTextureFormat depthFormat = WGPUTextureFormat_Undefined;
    WGPUBindGroupLayout groupLayout = nullptr;
    WebBindingLayout bindings;

    WGPUShaderModule module = nullptr;
    WGPUPipelineLayout pipelineLayout = nullptr;
    std::string vertexEntry;
    std::string fragmentEntry;
    RenderPipelineDescriptor descriptor;

    struct Variant
    {
        WGPUTextureFormat depthFormat = WGPUTextureFormat_Undefined;
        WGPURenderPipeline pipeline = nullptr;
    };

    Vector<Variant> variants;
};

// Null when WebGPU refuses it; the refusal is logged when the device reports it.
WGPURenderPipeline buildWebRenderPipeline(const WebRenderPipeline& state,
                                          WGPUTextureFormat depthFormat);

struct WebComputePipeline
{
    bool isValid() const { return pipeline != nullptr; }

    WGPUComputePipeline pipeline = nullptr;
    WGPUBindGroupLayout groupLayout = nullptr;
    WebBindingLayout bindings;
};

// What a pass has bound, gathered into the group at the next draw or dispatch.
struct WebBoundResources
{
    struct TextureBind
    {
        WGPUTextureView view = nullptr;
        TextureSampling sampling;
    };

    TextureBind textures[maxTextureSlots] = {};
    WebBufferBind buffers[webMaxBufferSlots] = {};
    UniformRange uniforms;
};

// Null when the device refuses it. Slots the layout declares and nothing bound
// get a placeholder.
WGPUBindGroup makeWebBindGroup(const WebBindingLayout& layout,
                               WGPUBindGroupLayout groupLayout,
                               const WebBoundResources& bound);

// The dynamic offset makeWebBindGroup's group takes, when the layout has one.
std::uint32_t webUniformOffset(const WebBindingLayout& layout,
                               const WebBoundResources& bound);

struct WebRenderEncoder
{
    WGPURenderPassEncoder pass = nullptr;
    WebGPUContext* context = nullptr;
    WebTextureData* target = nullptr;
    int width = 0;
    int height = 0;
    WGPUTextureFormat depthFormat = WGPUTextureFormat_Undefined;
    WebRenderPipeline* pipeline = nullptr;
};

struct WebComputeEncoder
{
    WGPUComputePassEncoder pass = nullptr;
    WebGPUContext* context = nullptr;
};

// Logged once per process, whatever calls it.
void reportWebUnsupported(const char* what);

// Logs a validation error that shows up for what the scope covered.
void pushWebErrorScope();
void popWebErrorScope(std::string label);
} // namespace eacp::GPU
