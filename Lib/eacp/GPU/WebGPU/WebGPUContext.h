#pragma once

#include "../Pipeline/RenderPipeline.h"
#include "../Shader/PipelineConstants.h"
#include "../Texture/Sampler.h"
#include "../Texture/Texture.h"

#include <eacp/Core/Utils/Containers.h>

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace eacp::GPU
{
class Device;
class WebGPUContext;

// The web backend: WebGPU through Emscripten's emdawnwebgpu port. The adapter
// and the device are asynchronous in a browser, so they are requested before
// main() by Web/WebGPU.js (a --pre-js every web executable that links eacp-gpu
// carries) and handed in as Module.preinitializedWebGPUDevice; a page or a
// node with no WebGPU leaves it unset, and every Device is then invalid.
//
// Waiting - a readback, a shader's compilation messages, a pipeline's
// validation - suspends the main thread through JSPI where the executable links
// eacp-core-nested-pump, and is skipped where it does not (webgpuCanWait).

// What a pipeline binds, read off its WGSL: every `@group(0) @binding(n)`
// declaration, one bind group layout entry each.
enum class WebGPUBindingKind
{
    UniformBuffer,
    ReadOnlyStorageBuffer,
    StorageBuffer,
    Texture,
    StorageTexture,
    Sampler
};

struct WebGPUBinding
{
    std::string name;
    int binding = 0;
    WebGPUBindingKind kind = WebGPUBindingKind::UniformBuffer;
    wgpu::TextureSampleType sampleType = wgpu::TextureSampleType::Float;
    wgpu::TextureViewDimension dimension = wgpu::TextureViewDimension::e2D;
    bool multisampled = false;
    wgpu::SamplerBindingType samplerType = wgpu::SamplerBindingType::Filtering;
    wgpu::TextureFormat storageFormat = wgpu::TextureFormat::Undefined;
    wgpu::StorageTextureAccess storageAccess = wgpu::StorageTextureAccess::WriteOnly;
};

Vector<WebGPUBinding> scanWgslBindings(std::string_view source);

// The bindings with each listed texture binding declared unfilterable-float,
// and every sampler that samples one of them made non-filtering. A WGSL
// texture_2d<f32> says nothing of filtering, but a bind group's layout must, and
// some float formats only bind as unfilterable (the 16-bit unorm formats
// always, the 32-bit floats without float32-filterable).
Vector<WebGPUBinding> withUnfilterableTextures(Vector<WebGPUBinding> bindings,
                                               const Vector<int>& unfilterable,
                                               std::string_view source);

// A compiled module and what it declares, which is what every pipeline built
// from the library reads.
struct WebGPUShaderProgram
{
    wgpu::ShaderModule module;
    Vector<WebGPUBinding> bindings;
    std::string source;
};

// A storage texture's format as a pass bound it, by binding number.
struct WebGPUStorageFormat
{
    int binding = 0;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;

    bool operator==(const WebGPUStorageFormat&) const = default;
};

// The program's WGSL with each named storage texture declared in the format
// given for its binding: WGSL fixes the format in the declaration, where MSL,
// HLSL and GLSL's unformatted writes take it from the texture bound.
std::string withStorageFormats(std::string source,
                               const Vector<WebGPUStorageFormat>& formats);

wgpu::ShaderModule makeWgslModule(const wgpu::Device& device,
                                  std::string_view source);

// The bind group layout and pipeline layout one program's bindings make, with
// the visibility of the stages that may see them.
struct WebGPUBindingLayout
{
    wgpu::BindGroupLayout group;
    wgpu::PipelineLayout pipeline;
    Vector<WebGPUBinding> bindings;
};

// What a pass bound that a compute kernel's WGSL could not know: the formats
// of its storage textures, and which float texture bindings hold a texture
// that may not be filtered.
struct WebGPUComputeVariantKey
{
    Vector<WebGPUStorageFormat> storageFormats;
    Vector<int> unfilterable;

    bool operator==(const WebGPUComputeVariantKey&) const = default;
};

// A compute pipeline, and the same kernel rebuilt for what a pass bound where
// that differs from what its WGSL declares.
struct WebGPUComputePipeline
{
    wgpu::ComputePipeline pipeline;
    WebGPUBindingLayout layout;

    std::function<const WebGPUComputePipeline*(const WebGPUComputeVariantKey&)>
        variantFor = [](const WebGPUComputeVariantKey&)
    { return static_cast<const WebGPUComputePipeline*>(nullptr); };
};

// A render pipeline is built once per depth format a pass attaches, which
// WebGPU fixes in the pipeline where Metal and Vulkan take it from the pass,
// and once per set of texture bindings a draw binds an unfilterable float
// texture to, each of those with its own layout.
struct WebGPURenderPipeline
{
    struct Variant
    {
        wgpu::TextureFormat depthFormat = wgpu::TextureFormat::Undefined;
        Vector<int> unfilterable;
        wgpu::RenderPipeline pipeline;
    };

    struct LayoutVariant
    {
        Vector<int> unfilterable;
        OwningPointer<WebGPUBindingLayout> layout;
    };

    wgpu::RenderPipeline pipelineFor(wgpu::TextureFormat depthFormat,
                                     const Vector<int>& unfilterable = {});

    // The layout of the declared bindings with those listed unfilterable:
    // `layout` itself for none, made once per set otherwise.
    const WebGPUBindingLayout& layoutFor(const Vector<int>& unfilterable);

    WebGPUBindingLayout layout;
    std::function<wgpu::RenderPipeline(wgpu::TextureFormat,
                                       const WebGPUBindingLayout&)>
        build = [](wgpu::TextureFormat, const WebGPUBindingLayout&)
    { return wgpu::RenderPipeline {}; };
    std::function<WebGPUBindingLayout(const Vector<int>&)> makeLayout =
        [](const Vector<int>&) { return WebGPUBindingLayout {}; };
    Vector<Variant> variants;
    Vector<LayoutVariant> layouts;
    bool valid = false;
};

// A pipeline's override values, keyed by each override's numeric @id, which
// is the PipelineConstant's id. Holds the key strings the entries point into.
struct WebGPUConstantEntries
{
    explicit WebGPUConstantEntries(const PipelineConstants& constants);

    WebGPUConstantEntries(const WebGPUConstantEntries&) = delete;
    WebGPUConstantEntries& operator=(const WebGPUConstantEntries&) = delete;

    Vector<std::string> keys;
    Vector<wgpu::ConstantEntry> entries;
};

// One command encoder: the recording a Frame or a CommandBuffer fills, and
// the uniform blocks it wrote on the way, which live until it is submitted.
struct WebGPURecording
{
    wgpu::CommandEncoder encoder;
    wgpu::Buffer uniformChunk;
    std::uint64_t uniformOffset = 0;
    std::uint64_t uniformCapacity = 0;
    bool passOpen = false;
};

struct WebGPUUniformSlice
{
    wgpu::Buffer buffer;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    bool isValid() const { return buffer != nullptr; }
};

struct WebGPUBufferData
{
    wgpu::Buffer buffer;
    std::uint64_t size = 0;
};

// A texture and the companions a render target draws with: the multisample
// image it resolves from and the depth plane beside it.
struct WebGPUTextureData
{
    wgpu::Texture texture;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    TextureFormat textureFormat = TextureFormat::RGBA8Unorm;
    wgpu::TextureView sampledView;
    wgpu::Texture multisample;
    wgpu::Texture depth;
    wgpu::TextureFormat depthFormat = wgpu::TextureFormat::Undefined;
    TextureType kind = TextureType::TwoD;
    int width = 0;
    int height = 0;
    int depthOrLayers = 1;
    int mipLevels = 1;
    int sampleCount = 1;
    bool renderTarget = false;
    bool computeWrite = false;
    bool sampleableDepth = false;

    bool isValid() const { return texture != nullptr; }
};

// The views a bind or an attachment takes, made as they are asked for. A level
// of -1 is every level; a slice of -1 every layer.
wgpu::TextureView webgpuSampledView(const WebGPUTextureData& data,
                                    int level = -1,
                                    bool asArray = false);
wgpu::TextureView webgpuStorageView(const WebGPUTextureData& data, int level = 0);
wgpu::TextureView webgpuDepthView(const WebGPUTextureData& data, int slice = -1);
wgpu::TextureView webgpuAttachmentView(const wgpu::Texture& texture,
                                       int level,
                                       int slice,
                                       int layers = 1);

// What a drawable Frame renders into: the canvas's current texture.
struct WebGPUDrawable
{
    WebGPUTextureData* target = nullptr;
};

// The open pass a ComputePass or RenderPass records into, and the recording
// it belongs to.
struct WebGPUComputeEncoder
{
    WebGPUContext* context = nullptr;
    WebGPURecording* recording = nullptr;
    wgpu::ComputePassEncoder pass;
};

struct WebGPURenderEncoder
{
    WebGPUContext* context = nullptr;
    WebGPURecording* recording = nullptr;
    wgpu::RenderPassEncoder pass;
    wgpu::TextureFormat depthFormat = wgpu::TextureFormat::Undefined;
    int width = 0;
    int height = 0;
};

class WebGPUContext;

// What a pass has bound, by binding number, made into a bind group for the
// pipeline's layout at the draw or dispatch: a declared binding nothing was
// bound to takes an empty resource of its kind.
class WebGPUBindingTable
{
public:
    void bindBuffer(int binding,
                    const wgpu::Buffer& buffer,
                    std::uint64_t offset,
                    std::uint64_t size);
    void
        bindView(int binding, const wgpu::TextureView& view, bool filterable = true);
    void bindSampler(int binding, const wgpu::Sampler& sampler, bool filtering);

    bool isDirty() const { return dirty; }

    // The declared float texture bindings this table holds an unfilterable
    // texture at, which a pipeline variant has to declare unfilterable-float.
    Vector<int> unfilterableBindings(const Vector<WebGPUBinding>& declared) const;

    // Whether the layout binds `buffer` writable (read_write storage) anywhere.
    bool bindsWritable(const WebGPUBindingLayout& layout,
                       const wgpu::Buffer& buffer) const;

    wgpu::BindGroup makeGroup(WebGPUContext& context,
                              const WebGPUBindingLayout& layout);

private:
    struct Resource
    {
        int binding = 0;
        wgpu::Buffer buffer;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        wgpu::TextureView view;
        wgpu::Sampler sampler;
        bool filtering = false;
        bool filterable = true;
    };

    Resource& resourceAt(int binding);
    const Resource* findResource(int binding) const;

    Vector<Resource> resources;
    bool dirty = true;
};

class WebGPUContext
{
public:
    WebGPUContext();

    bool isValid() const;

    const wgpu::Device& getDevice() const { return device; }
    const wgpu::Queue& getQueue() const { return queue; }
    const wgpu::Limits& getLimits() const { return limits; }
    const std::string& getAdapterName() const { return adapterName; }

    bool hasFeature(wgpu::FeatureName feature) const;

    // Whether a texture of this format may be sampled through a filtering
    // sampler: not a 16-bit unorm one, which WebGPU (texture-formats-tier1)
    // only binds as unfilterable-float, nor a 32-bit float one without
    // float32-filterable.
    bool isFilterable(wgpu::TextureFormat format) const;

    // A small buffer an indirect dispatch reads its arguments from when the
    // buffer they live in is also bound writable to the same dispatch, which
    // WebGPU's usage-scope rule forbids. The queue runs in order, so one is
    // enough: each copy lands after the dispatch that read the one before.
    const wgpu::Buffer& getIndirectScratch();

    // Whether a storage bind may start here: on the device's offset grid,
    // Device::storageBufferOffsetAlignment. A bind off it binds nothing.
    bool isStorageOffset(std::uint64_t offset) const;

    OwningPointer<WebGPURecording> beginRecording();
    void submit(WebGPURecording& recording);

    WebGPUUniformSlice uploadUniforms(WebGPURecording& recording,
                                      const void* data,
                                      std::size_t bytes);

    wgpu::Sampler getSampler(TextureSampling sampling);
    wgpu::Sampler makeSampler(const SamplerDescriptor& descriptor);

    // Resources a bind group falls back on for a binding nothing was bound to,
    // so a slot left empty reads zeroes as it does elsewhere.
    const wgpu::Buffer& getEmptyBuffer();
    wgpu::TextureView getEmptyTextureView(const WebGPUBinding& binding);
    wgpu::Sampler getEmptySampler(wgpu::SamplerBindingType type);

    WebGPUBindingLayout makeBindingLayout(const Vector<WebGPUBinding>& bindings,
                                          bool compute);

    // Blocks until everything submitted has finished, where waiting is possible.
    void waitIdle();

    // Runs a block in an error scope and, where it can wait, says whether the
    // device reported a validation error from it.
    bool succeeds(const std::function<void()>& block,
                  std::string* message = nullptr);

private:
    wgpu::Instance instance;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Limits limits;
    std::string adapterName;

    std::array<wgpu::Sampler, samplingConfigurations> samplers;
    wgpu::Buffer emptyBuffer;
    wgpu::Buffer indirectScratch;
    Vector<std::pair<std::string, wgpu::TextureView>> emptyViews;
    wgpu::Sampler emptyFiltering;
    wgpu::Sampler emptyComparison;
};

WebGPUContext& getWebGPUContext(const Device& device);

// Whether this executable can suspend for a callback (eacp-core-nested-pump).
bool webgpuCanWait();

// Suspends until `done` is set. Returns at once, false, where it cannot wait.
bool webgpuWaitUntil(const bool& done);

// What a callback runs to end a webgpuWaitUntil.
void webgpuWake(bool& done);

wgpu::TextureFormat toWGPUFormat(TextureFormat format);
wgpu::TextureFormat toWGPUFormat(PixelFormat format);
wgpu::CompareFunction toWGPUCompare(CompareFunction compare);
wgpu::TextureViewDimension sampledDimension(TextureType kind);

std::uint64_t alignTo(std::uint64_t value, std::uint64_t alignment);

// A buffer's byte count as the host's size_t, which is 32 bits on wasm32:
// anything mapped or copied into memory already fits in the address space.
std::size_t toHostSize(std::uint64_t bytes);
} // namespace eacp::GPU
