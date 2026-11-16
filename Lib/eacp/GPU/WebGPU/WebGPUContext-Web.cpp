#include "WebGPUContext.h"

#include "../Codegen/ShaderBindings.h"
#include "../Device/Device.h"

#include <eacp/Core/Threads/EventLoop-Web.h>
#include <eacp/Core/Utils/Logging.h>

#include <emscripten/em_js.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>

// clang-format off
EM_JS(int, eacpWebGPUHasDevice, (), {
    return Module["preinitializedWebGPUDevice"] ? 1 : 0;
});

EM_JS_DEPS(eacpWebGPUAdapterNameDeps, "$stringToNewUTF8");

EM_JS(char*, eacpWebGPUAdapterName, (), {
    return stringToNewUTF8(Module["eacpWebGPUAdapterName"] || "WebGPU");
});
// clang-format on

namespace eacp::GPU
{
namespace
{
constexpr std::uint64_t uniformChunkBytes = 64 * 1024;
constexpr std::uint64_t uniformBlockAlignment = 16;
constexpr std::uint64_t emptyBufferBytes = 256;

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);

    while (!text.empty()
           && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);

    return text;
}

int parseNumberAfter(std::string_view text, std::string_view marker)
{
    auto at = text.find(marker);

    if (at == std::string_view::npos)
        return -1;

    auto digits = text.substr(at + marker.size());
    auto value = -1;
    std::from_chars(digits.data(), digits.data() + digits.size(), value);
    return value;
}

wgpu::TextureViewDimension dimensionNamed(std::string_view name)
{
    if (name == "1d")
        return wgpu::TextureViewDimension::e1D;

    if (name == "2d_array")
        return wgpu::TextureViewDimension::e2DArray;

    if (name == "cube")
        return wgpu::TextureViewDimension::Cube;

    if (name == "cube_array")
        return wgpu::TextureViewDimension::CubeArray;

    if (name == "3d")
        return wgpu::TextureViewDimension::e3D;

    return wgpu::TextureViewDimension::e2D;
}

wgpu::TextureSampleType sampleTypeNamed(std::string_view name)
{
    if (name == "u32")
        return wgpu::TextureSampleType::Uint;

    if (name == "i32")
        return wgpu::TextureSampleType::Sint;

    return wgpu::TextureSampleType::Float;
}

struct StorageFormatName
{
    std::string_view name;
    wgpu::TextureFormat format;
};

constexpr StorageFormatName storageFormatNames[] = {
    {"rgba8unorm", wgpu::TextureFormat::RGBA8Unorm},
    {"rgba8snorm", wgpu::TextureFormat::RGBA8Snorm},
    {"rgba8uint", wgpu::TextureFormat::RGBA8Uint},
    {"rgba8sint", wgpu::TextureFormat::RGBA8Sint},
    {"rgba16uint", wgpu::TextureFormat::RGBA16Uint},
    {"rgba16sint", wgpu::TextureFormat::RGBA16Sint},
    {"rgba16float", wgpu::TextureFormat::RGBA16Float},
    {"rgba16unorm", wgpu::TextureFormat::RGBA16Unorm},
    {"r32uint", wgpu::TextureFormat::R32Uint},
    {"r32sint", wgpu::TextureFormat::R32Sint},
    {"r32float", wgpu::TextureFormat::R32Float},
    {"rg32uint", wgpu::TextureFormat::RG32Uint},
    {"rg32sint", wgpu::TextureFormat::RG32Sint},
    {"rg32float", wgpu::TextureFormat::RG32Float},
    {"rgba32uint", wgpu::TextureFormat::RGBA32Uint},
    {"rgba32sint", wgpu::TextureFormat::RGBA32Sint},
    {"rgba32float", wgpu::TextureFormat::RGBA32Float},
    {"bgra8unorm", wgpu::TextureFormat::BGRA8Unorm},
    {"r8unorm", wgpu::TextureFormat::R8Unorm},
    {"r8uint", wgpu::TextureFormat::R8Uint},
    {"r8sint", wgpu::TextureFormat::R8Sint},
    {"rg8unorm", wgpu::TextureFormat::RG8Unorm},
    {"rg8uint", wgpu::TextureFormat::RG8Uint},
    {"rg8sint", wgpu::TextureFormat::RG8Sint},
    {"r16float", wgpu::TextureFormat::R16Float},
    {"r16uint", wgpu::TextureFormat::R16Uint},
    {"r16sint", wgpu::TextureFormat::R16Sint},
    {"r16unorm", wgpu::TextureFormat::R16Unorm},
    {"rg16float", wgpu::TextureFormat::RG16Float},
    {"rg16uint", wgpu::TextureFormat::RG16Uint},
    {"rg16sint", wgpu::TextureFormat::RG16Sint},
    {"rg16unorm", wgpu::TextureFormat::RG16Unorm},
    {"rgb10a2unorm", wgpu::TextureFormat::RGB10A2Unorm},
    {"rgb10a2uint", wgpu::TextureFormat::RGB10A2Uint},
    {"rg11b10ufloat", wgpu::TextureFormat::RG11B10Ufloat},
};

wgpu::TextureFormat storageFormatNamed(std::string_view name)
{
    for (const auto& entry: storageFormatNames)
        if (entry.name == name)
            return entry.format;

    return wgpu::TextureFormat::Undefined;
}

wgpu::StorageTextureAccess storageAccessNamed(std::string_view name)
{
    if (name == "read")
        return wgpu::StorageTextureAccess::ReadOnly;

    if (name == "read_write")
        return wgpu::StorageTextureAccess::ReadWrite;

    return wgpu::StorageTextureAccess::WriteOnly;
}

// The text between the first '<' and its '>', split at the first comma.
std::pair<std::string_view, std::string_view>
    templateArguments(std::string_view type)
{
    auto open = type.find('<');
    auto close = type.rfind('>');

    if (open == std::string_view::npos || close == std::string_view::npos
        || close < open)
        return {};

    auto inside = type.substr(open + 1, close - open - 1);
    auto comma = inside.find(',');

    if (comma == std::string_view::npos)
        return {trimmed(inside), {}};

    return {trimmed(inside.substr(0, comma)), trimmed(inside.substr(comma + 1))};
}

void classifyTexture(std::string_view type, WebGPUBinding& binding)
{
    constexpr auto storagePrefix = std::string_view {"texture_storage_"};
    constexpr auto depthPrefix = std::string_view {"texture_depth_"};
    constexpr auto multisampledPrefix = std::string_view {"texture_multisampled_"};

    auto shape = type.substr(0, type.find('<'));
    auto [first, second] = templateArguments(type);

    if (shape.starts_with(storagePrefix))
    {
        binding.kind = WebGPUBindingKind::StorageTexture;
        binding.dimension = dimensionNamed(shape.substr(storagePrefix.size()));
        binding.storageFormat = storageFormatNamed(first);
        binding.storageAccess = storageAccessNamed(second);
        return;
    }

    binding.kind = WebGPUBindingKind::Texture;

    if (shape.starts_with(depthPrefix))
    {
        auto rest = shape.substr(depthPrefix.size());
        binding.sampleType = wgpu::TextureSampleType::Depth;
        binding.multisampled = rest.starts_with("multisampled");
        binding.dimension = binding.multisampled ? wgpu::TextureViewDimension::e2D
                                                 : dimensionNamed(rest);
        return;
    }

    if (shape.starts_with(multisampledPrefix))
    {
        binding.multisampled = true;
        binding.dimension = wgpu::TextureViewDimension::e2D;
        binding.sampleType = sampleTypeNamed(first) == wgpu::TextureSampleType::Float
                                 ? wgpu::TextureSampleType::UnfilterableFloat
                                 : sampleTypeNamed(first);
        return;
    }

    binding.dimension =
        dimensionNamed(shape.substr(std::string_view {"texture_"}.size()));
    binding.sampleType = sampleTypeNamed(first);
}

std::optional<WebGPUBinding> parseBindingLine(std::string_view line)
{
    if (line.find("@group(0)") == std::string_view::npos)
        return std::nullopt;

    auto binding = WebGPUBinding {};
    binding.binding = parseNumberAfter(line, "@binding(");

    auto var = line.find("var");

    if (binding.binding < 0 || var == std::string_view::npos)
        return std::nullopt;

    auto declaration = line.substr(var + 3);
    auto addressSpace = std::string_view {};

    if (declaration.starts_with("<"))
    {
        auto close = declaration.find('>');

        if (close == std::string_view::npos)
            return std::nullopt;

        addressSpace = trimmed(declaration.substr(1, close - 1));
        declaration = declaration.substr(close + 1);
    }

    auto colon = declaration.find(':');
    auto end = declaration.rfind(';');

    if (colon == std::string_view::npos)
        return std::nullopt;

    binding.name = std::string(trimmed(declaration.substr(0, colon)));

    auto type = trimmed(declaration.substr(
        colon + 1, end == std::string_view::npos ? end : end - colon - 1));

    if (addressSpace == "uniform")
    {
        binding.kind = WebGPUBindingKind::UniformBuffer;
        return binding;
    }

    if (addressSpace.starts_with("storage"))
    {
        binding.kind = addressSpace.find("read_write") != std::string_view::npos
                           ? WebGPUBindingKind::StorageBuffer
                           : WebGPUBindingKind::ReadOnlyStorageBuffer;
        return binding;
    }

    if (type.starts_with("sampler_comparison"))
    {
        binding.kind = WebGPUBindingKind::Sampler;
        binding.samplerType = wgpu::SamplerBindingType::Comparison;
        return binding;
    }

    if (type.starts_with("sampler"))
    {
        binding.kind = WebGPUBindingKind::Sampler;
        return binding;
    }

    if (type.starts_with("texture_"))
    {
        classifyTexture(type, binding);
        return binding;
    }

    return std::nullopt;
}

const WebGPUBinding* findBinding(const std::vector<WebGPUBinding>& bindings,
                                 int number)
{
    for (const auto& binding: bindings)
        if (binding.binding == number)
            return &binding;

    return nullptr;
}

// A filtering sampler beside a texture WebGPU will not filter fails the
// pipeline, so the sampler of such a slot is declared non-filtering.
bool isUnfilterable(const WebGPUBinding& binding)
{
    return binding.kind == WebGPUBindingKind::Texture
           && binding.sampleType != wgpu::TextureSampleType::Float;
}

// Whether the source samples `texture` through `sampler` anywhere: a texture
// builtin takes the two as its first arguments, side by side.
bool isSampledThrough(std::string_view source,
                      const WebGPUBinding& texture,
                      const WebGPUBinding& sampler)
{
    return source.find(texture.name + ", " + sampler.name) != std::string_view::npos;
}

// A sampler-only slot is declared as a plain sampler, but one a comparison
// samples through has to be bound as a comparison sampler.
bool isComparedThrough(std::string_view source,
                       const WebGPUBinding& texture,
                       const WebGPUBinding& sampler)
{
    const auto pair = texture.name + ", " + sampler.name;

    for (auto call: {"textureSampleCompare(",
                     "textureSampleCompareLevel(",
                     "textureGatherCompare("})
        if (source.find(call + pair) != std::string_view::npos)
            return true;

    return false;
}

void pairSamplersWithTextures(std::vector<WebGPUBinding>& bindings,
                              std::string_view source)
{
    for (auto& binding: bindings)
    {
        if (binding.kind != WebGPUBindingKind::Sampler
            || binding.samplerType != wgpu::SamplerBindingType::Filtering)
            continue;

        const auto slot = binding.binding - webgpuSamplerBinding(0);
        const auto* sameSlot = findBinding(bindings, webgpuTextureBinding(slot));

        if (sameSlot != nullptr && isUnfilterable(*sameSlot))
            binding.samplerType = wgpu::SamplerBindingType::NonFiltering;

        for (const auto& texture: bindings)
            if (isUnfilterable(texture)
                && isSampledThrough(source, texture, binding))
                binding.samplerType = wgpu::SamplerBindingType::NonFiltering;

        for (const auto& texture: bindings)
            if (isComparedThrough(source, texture, binding))
                binding.samplerType = wgpu::SamplerBindingType::Comparison;
    }
}

wgpu::AddressMode toWGPUAddressMode(SamplerAddressMode mode)
{
    switch (mode)
    {
        case SamplerAddressMode::Repeat:
            return wgpu::AddressMode::Repeat;
        case SamplerAddressMode::MirrorRepeat:
            return wgpu::AddressMode::MirrorRepeat;
        default:
            return wgpu::AddressMode::ClampToEdge;
    }
}

wgpu::FilterMode toWGPUFilter(SamplerFilter filter)
{
    return filter == SamplerFilter::Linear ? wgpu::FilterMode::Linear
                                           : wgpu::FilterMode::Nearest;
}

wgpu::ShaderStage visibilityOf(const WebGPUBinding& binding, bool compute)
{
    if (compute)
        return wgpu::ShaderStage::Compute;

    auto writable =
        binding.kind == WebGPUBindingKind::StorageBuffer
        || (binding.kind == WebGPUBindingKind::StorageTexture
            && binding.storageAccess != wgpu::StorageTextureAccess::ReadOnly);

    if (writable)
        return wgpu::ShaderStage::Fragment;

    return wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
}

wgpu::BindGroupLayoutEntry layoutEntryFor(const WebGPUBinding& binding, bool compute)
{
    auto entry = wgpu::BindGroupLayoutEntry {};
    entry.binding = static_cast<std::uint32_t>(binding.binding);
    entry.visibility = visibilityOf(binding, compute);

    switch (binding.kind)
    {
        case WebGPUBindingKind::UniformBuffer:
            entry.buffer.type = wgpu::BufferBindingType::Uniform;
            break;
        case WebGPUBindingKind::ReadOnlyStorageBuffer:
            entry.buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
            break;
        case WebGPUBindingKind::StorageBuffer:
            entry.buffer.type = wgpu::BufferBindingType::Storage;
            break;
        case WebGPUBindingKind::Texture:
            entry.texture.sampleType = binding.sampleType;
            entry.texture.viewDimension = binding.dimension;
            entry.texture.multisampled = binding.multisampled;
            break;
        case WebGPUBindingKind::StorageTexture:
            entry.storageTexture.access = binding.storageAccess;
            entry.storageTexture.format = binding.storageFormat;
            entry.storageTexture.viewDimension = binding.dimension;
            break;
        case WebGPUBindingKind::Sampler:
            entry.sampler.type = binding.samplerType;
            break;
    }

    return entry;
}

int layersOf(wgpu::TextureViewDimension dimension)
{
    if (dimension == wgpu::TextureViewDimension::Cube
        || dimension == wgpu::TextureViewDimension::CubeArray)
        return 6;

    return 1;
}

std::string emptyViewKey(const WebGPUBinding& binding)
{
    return std::to_string(static_cast<int>(binding.kind)) + ":"
           + std::to_string(static_cast<int>(binding.sampleType)) + ":"
           + std::to_string(static_cast<int>(binding.dimension)) + ":"
           + std::to_string(binding.multisampled ? 1 : 0) + ":"
           + std::to_string(static_cast<int>(binding.storageFormat));
}

wgpu::TextureFormat emptyTextureFormat(const WebGPUBinding& binding)
{
    if (binding.kind == WebGPUBindingKind::StorageTexture)
        return binding.storageFormat;

    switch (binding.sampleType)
    {
        case wgpu::TextureSampleType::Depth:
            return wgpu::TextureFormat::Depth32Float;
        case wgpu::TextureSampleType::Uint:
            return wgpu::TextureFormat::RGBA8Uint;
        case wgpu::TextureSampleType::Sint:
            return wgpu::TextureFormat::RGBA8Sint;
        default:
            return wgpu::TextureFormat::RGBA8Unorm;
    }
}

double constantValueOf(const PipelineConstant& constant)
{
    switch (constant.type)
    {
        case PipelineConstantType::Bool:
            return constant.asBool() ? 1.0 : 0.0;
        case PipelineConstantType::Int:
            return static_cast<double>(constant.asInt());
        case PipelineConstantType::UInt:
            return static_cast<double>(constant.asUInt());
        default:
            return static_cast<double>(constant.asFloat());
    }
}
} // namespace

WebGPUConstantEntries::WebGPUConstantEntries(const PipelineConstants& constants)
{
    for (const auto& constant: constants.values)
        keys.push_back(std::to_string(constant.id));

    auto index = 0;

    for (const auto& constant: constants.values)
    {
        const auto& key = keys[static_cast<std::size_t>(index++)];
        auto entry = wgpu::ConstantEntry {};
        entry.key = wgpu::StringView {key.data(), key.size()};
        entry.value = constantValueOf(constant);
        entries.push_back(entry);
    }
}

std::vector<WebGPUBinding> scanWgslBindings(std::string_view source)
{
    const auto original = source;
    auto bindings = std::vector<WebGPUBinding> {};

    while (!source.empty())
    {
        auto end = source.find('\n');
        auto line = source.substr(0, end);

        if (line.find("@binding(") != std::string_view::npos)
            if (auto binding = parseBindingLine(line))
                if (findBinding(bindings, binding->binding) == nullptr)
                    bindings.push_back(*binding);

        if (end == std::string_view::npos)
            break;

        source.remove_prefix(end + 1);
    }

    pairSamplersWithTextures(bindings, original);

    std::sort(bindings.begin(),
              bindings.end(),
              [](const WebGPUBinding& a, const WebGPUBinding& b)
              { return a.binding < b.binding; });

    return bindings;
}

WebGPUBindingTable::Resource& WebGPUBindingTable::resourceAt(int binding)
{
    dirty = true;

    for (auto& resource: resources)
        if (resource.binding == binding)
            return resource;

    resources.push_back(Resource {binding});
    return resources.back();
}

const WebGPUBindingTable::Resource*
    WebGPUBindingTable::findResource(int binding) const
{
    for (const auto& resource: resources)
        if (resource.binding == binding)
            return &resource;

    return nullptr;
}

void WebGPUBindingTable::bindBuffer(int binding,
                                    const wgpu::Buffer& buffer,
                                    std::uint64_t offset,
                                    std::uint64_t size)
{
    auto& resource = resourceAt(binding);
    resource.buffer = buffer;
    resource.offset = offset;
    resource.size = size;
}

void WebGPUBindingTable::bindView(int binding, const wgpu::TextureView& view)
{
    resourceAt(binding).view = view;
}

void WebGPUBindingTable::bindSampler(int binding,
                                     const wgpu::Sampler& sampler,
                                     bool filtering)
{
    auto& resource = resourceAt(binding);
    resource.sampler = sampler;
    resource.filtering = filtering;
}

wgpu::BindGroup WebGPUBindingTable::makeGroup(WebGPUContext& context,
                                              const WebGPUBindingLayout& layout)
{
    dirty = false;

    if (layout.group == nullptr)
        return {};

    auto entries = std::vector<wgpu::BindGroupEntry> {};

    for (const auto& binding: layout.bindings)
    {
        auto entry = wgpu::BindGroupEntry {};
        entry.binding = static_cast<std::uint32_t>(binding.binding);
        const auto* bound = findResource(binding.binding);

        switch (binding.kind)
        {
            case WebGPUBindingKind::UniformBuffer:
            case WebGPUBindingKind::ReadOnlyStorageBuffer:
            case WebGPUBindingKind::StorageBuffer:
                if (bound != nullptr && bound->buffer != nullptr)
                {
                    entry.buffer = bound->buffer;
                    entry.offset = bound->offset;
                    entry.size = bound->size;
                }
                else
                {
                    entry.buffer = context.getEmptyBuffer();
                    entry.size = emptyBufferBytes;
                }
                break;
            case WebGPUBindingKind::Texture:
            case WebGPUBindingKind::StorageTexture:
                entry.textureView = bound != nullptr && bound->view != nullptr
                                        ? bound->view
                                        : context.getEmptyTextureView(binding);
                break;
            case WebGPUBindingKind::Sampler:
            {
                const auto usable =
                    bound != nullptr && bound->sampler != nullptr
                    && (binding.samplerType != wgpu::SamplerBindingType::NonFiltering
                        || !bound->filtering);

                entry.sampler = usable
                                    ? bound->sampler
                                    : context.getEmptySampler(binding.samplerType);
                break;
            }
        }

        entries.push_back(entry);
    }

    auto descriptor = wgpu::BindGroupDescriptor {};
    descriptor.layout = layout.group;
    descriptor.entryCount = entries.size();
    descriptor.entries = entries.data();
    return context.getDevice().CreateBindGroup(&descriptor);
}

std::string withStorageFormats(std::string source,
                               const std::vector<WebGPUStorageFormat>& formats)
{
    constexpr auto storagePrefix = std::string_view {"texture_storage_"};

    for (const auto& wanted: formats)
    {
        auto name = std::string_view {};

        for (const auto& entry: storageFormatNames)
            if (entry.format == wanted.format)
                name = entry.name;

        const auto marker = "@binding(" + std::to_string(wanted.binding) + ")";
        const auto line = source.find(marker);

        if (name.empty() || line == std::string::npos)
            continue;

        const auto type = source.find(storagePrefix, line);
        const auto lineEnd = source.find('\n', line);

        if (type == std::string::npos
            || (lineEnd != std::string::npos && type > lineEnd))
            continue;

        const auto open = source.find('<', type);
        const auto comma = source.find(',', open);

        if (open == std::string::npos || comma == std::string::npos)
            continue;

        source.replace(open + 1, comma - open - 1, name);
    }

    return source;
}

wgpu::ShaderModule makeWgslModule(const wgpu::Device& device,
                                  std::string_view source)
{
    auto wgsl = wgpu::ShaderSourceWGSL {};
    wgsl.code = wgpu::StringView {source.data(), source.size()};

    auto descriptor = wgpu::ShaderModuleDescriptor {};
    descriptor.nextInChain = &wgsl;
    return device.CreateShaderModule(&descriptor);
}

bool webgpuCanWait()
{
    return eacpWebCanSuspend();
}

bool webgpuWaitUntil(const bool& done)
{
    if (done)
        return true;

    if (!webgpuCanWait())
        return false;

    while (!done)
        eacpWebSuspendUntilWoken(-1.0);

    return true;
}

void webgpuWake(bool& done)
{
    done = true;
    eacpWebWakeSuspended();
}

std::uint64_t alignTo(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

wgpu::RenderPipeline
    WebGPURenderPipeline::pipelineFor(wgpu::TextureFormat depthFormat)
{
    for (const auto& [format, pipeline]: variants)
        if (format == depthFormat)
            return pipeline;

    auto pipeline = build(depthFormat);
    variants.emplace_back(depthFormat, pipeline);
    return pipeline;
}

WebGPUContext::WebGPUContext()
{
    if (eacpWebGPUHasDevice() == 0)
        return;

    device = wgpu::Device::Acquire(emscripten_webgpu_get_device());

    if (device == nullptr)
        return;

    queue = device.GetQueue();
    device.GetLimits(&limits);

    auto* name = eacpWebGPUAdapterName();
    adapterName = name != nullptr ? name : "WebGPU";
    std::free(name);
}

bool WebGPUContext::isValid() const
{
    return device != nullptr;
}

bool WebGPUContext::hasFeature(wgpu::FeatureName feature) const
{
    return device != nullptr && device.HasFeature(feature);
}

bool WebGPUContext::isStorageOffset(std::uint64_t offset) const
{
    const auto grid =
        std::max<std::uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
    return offset % grid == 0;
}

std::unique_ptr<WebGPURecording> WebGPUContext::beginRecording()
{
    if (!isValid())
        return nullptr;

    auto recording = std::make_unique<WebGPURecording>();
    recording->encoder = device.CreateCommandEncoder();
    return recording;
}

void WebGPUContext::submit(WebGPURecording& recording)
{
    if (recording.encoder == nullptr)
        return;

    auto commands = recording.encoder.Finish();
    queue.Submit(1, &commands);

    recording.encoder = nullptr;
    recording.uniformChunk = nullptr;
    recording.uniformOffset = 0;
    recording.uniformCapacity = 0;
}

WebGPUUniformSlice WebGPUContext::uploadUniforms(WebGPURecording& recording,
                                                 const void* data,
                                                 std::size_t bytes)
{
    if (!isValid() || data == nullptr || bytes == 0)
        return {};

    const auto size = alignTo(bytes, uniformBlockAlignment);
    const auto alignment =
        std::max<std::uint64_t>(limits.minUniformBufferOffsetAlignment, 16);

    if (recording.uniformChunk == nullptr
        || recording.uniformOffset + size > recording.uniformCapacity)
    {
        auto descriptor = wgpu::BufferDescriptor {};
        descriptor.size = std::max(uniformChunkBytes, size);
        descriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;

        recording.uniformChunk = device.CreateBuffer(&descriptor);
        recording.uniformOffset = 0;
        recording.uniformCapacity = descriptor.size;
    }

    auto padded = std::vector<std::uint8_t>(size, 0);
    std::memcpy(padded.data(), data, bytes);

    auto slice =
        WebGPUUniformSlice {recording.uniformChunk, recording.uniformOffset, size};
    queue.WriteBuffer(slice.buffer, slice.offset, padded.data(), padded.size());

    recording.uniformOffset = alignTo(recording.uniformOffset + size, alignment);
    return slice;
}

wgpu::Sampler WebGPUContext::makeSampler(const SamplerDescriptor& descriptor)
{
    if (!isValid())
        return {};

    auto info = wgpu::SamplerDescriptor {};
    info.addressModeU = toWGPUAddressMode(descriptor.addressU);
    info.addressModeV = toWGPUAddressMode(descriptor.addressV);
    info.addressModeW = toWGPUAddressMode(descriptor.addressW);
    info.minFilter = toWGPUFilter(descriptor.minFilter);
    info.magFilter = toWGPUFilter(descriptor.magFilter);
    info.mipmapFilter = descriptor.mipFilter == SamplerMipFilter::Linear
                            ? wgpu::MipmapFilterMode::Linear
                            : wgpu::MipmapFilterMode::Nearest;
    info.lodMinClamp = descriptor.lodMinClamp;
    info.lodMaxClamp = descriptor.mipFilter == SamplerMipFilter::None
                           ? 0.f
                           : std::min(descriptor.lodMaxClamp, 32.f);

    auto fullyLinear = descriptor.minFilter == SamplerFilter::Linear
                       && descriptor.magFilter == SamplerFilter::Linear
                       && descriptor.mipFilter == SamplerMipFilter::Linear;
    info.maxAnisotropy =
        fullyLinear
            ? static_cast<std::uint16_t>(std::clamp(descriptor.maxAnisotropy, 1, 16))
            : 1;

    if (descriptor.compare)
        info.compare = toWGPUCompare(*descriptor.compare);

    return device.CreateSampler(&info);
}

wgpu::Sampler WebGPUContext::getSampler(TextureSampling sampling)
{
    auto& sampler = samplers[static_cast<std::size_t>(samplingIndex(sampling))];

    if (sampler == nullptr)
        sampler = makeSampler(SamplerDescriptor::from(sampling));

    return sampler;
}

const wgpu::Buffer& WebGPUContext::getEmptyBuffer()
{
    if (emptyBuffer == nullptr && isValid())
    {
        auto descriptor = wgpu::BufferDescriptor {};
        descriptor.size = emptyBufferBytes;
        descriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::Storage
                           | wgpu::BufferUsage::CopyDst;
        emptyBuffer = device.CreateBuffer(&descriptor);
    }

    return emptyBuffer;
}

wgpu::TextureView WebGPUContext::getEmptyTextureView(const WebGPUBinding& binding)
{
    const auto key = emptyViewKey(binding);

    for (const auto& [name, view]: emptyViews)
        if (name == key)
            return view;

    if (!isValid())
        return {};

    auto descriptor = wgpu::TextureDescriptor {};
    descriptor.format = emptyTextureFormat(binding);
    descriptor.size = {
        1, 1, static_cast<std::uint32_t>(layersOf(binding.dimension))};
    descriptor.dimension = binding.dimension == wgpu::TextureViewDimension::e3D
                               ? wgpu::TextureDimension::e3D
                               : wgpu::TextureDimension::e2D;
    descriptor.sampleCount = binding.multisampled ? 4u : 1u;
    descriptor.usage = binding.kind == WebGPUBindingKind::StorageTexture
                           ? wgpu::TextureUsage::StorageBinding
                           : wgpu::TextureUsage::TextureBinding;

    if (binding.multisampled)
        descriptor.usage |= wgpu::TextureUsage::RenderAttachment;

    auto texture = device.CreateTexture(&descriptor);

    auto viewDescriptor = wgpu::TextureViewDescriptor {};
    viewDescriptor.dimension = binding.dimension;
    auto view = texture.CreateView(&viewDescriptor);

    emptyViews.emplace_back(key, view);
    return view;
}

wgpu::Sampler WebGPUContext::getEmptySampler(wgpu::SamplerBindingType type)
{
    auto& sampler = type == wgpu::SamplerBindingType::Comparison ? emptyComparison
                                                                 : emptyFiltering;

    if (sampler == nullptr && isValid())
    {
        auto descriptor = SamplerDescriptor {};

        if (type == wgpu::SamplerBindingType::Comparison)
            descriptor.compare = CompareFunction::LessEqual;

        sampler = makeSampler(descriptor);
    }

    return sampler;
}

WebGPUBindingLayout
    WebGPUContext::makeBindingLayout(const std::vector<WebGPUBinding>& bindings,
                                     bool compute)
{
    auto layout = WebGPUBindingLayout {};
    layout.bindings = bindings;

    if (!isValid())
        return layout;

    auto entries = std::vector<wgpu::BindGroupLayoutEntry> {};

    for (const auto& binding: bindings)
        entries.push_back(layoutEntryFor(binding, compute));

    auto groupDescriptor = wgpu::BindGroupLayoutDescriptor {};
    groupDescriptor.entryCount = entries.size();
    groupDescriptor.entries = entries.data();
    layout.group = device.CreateBindGroupLayout(&groupDescriptor);

    auto pipelineDescriptor = wgpu::PipelineLayoutDescriptor {};
    pipelineDescriptor.bindGroupLayoutCount = 1;
    pipelineDescriptor.bindGroupLayouts = &layout.group;
    layout.pipeline = device.CreatePipelineLayout(&pipelineDescriptor);

    return layout;
}

void WebGPUContext::waitIdle()
{
    if (!isValid() || !webgpuCanWait())
        return;

    auto done = false;

    queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
                              [&done](wgpu::QueueWorkDoneStatus, wgpu::StringView)
                              { webgpuWake(done); });

    webgpuWaitUntil(done);
}

bool WebGPUContext::succeeds(const std::function<void()>& block,
                             std::string* message)
{
    if (!isValid())
        return false;

    if (!webgpuCanWait())
    {
        block();
        return true;
    }

    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    block();

    auto done = false;
    auto failed = false;

    device.PopErrorScope(
        wgpu::CallbackMode::AllowSpontaneous,
        [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView text)
        {
            failed = type != wgpu::ErrorType::NoError;

            if (failed && message != nullptr)
                *message = std::string(text.data, text.length);

            webgpuWake(done);
        });

    webgpuWaitUntil(done);
    return !failed;
}

WebGPUContext& getWebGPUContext(const Device& device)
{
    return *static_cast<WebGPUContext*>(device.nativeContext());
}

wgpu::TextureFormat toWGPUFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
            return wgpu::TextureFormat::RGBA8Unorm;
        case TextureFormat::BGRA8Unorm:
            return wgpu::TextureFormat::BGRA8Unorm;
        case TextureFormat::R8Unorm:
            return wgpu::TextureFormat::R8Unorm;
        case TextureFormat::RG8Unorm:
            return wgpu::TextureFormat::RG8Unorm;
        case TextureFormat::RGBA16Float:
            return wgpu::TextureFormat::RGBA16Float;
        case TextureFormat::RGBA32Float:
            return wgpu::TextureFormat::RGBA32Float;
        case TextureFormat::R32Float:
            return wgpu::TextureFormat::R32Float;
        case TextureFormat::R16Float:
            return wgpu::TextureFormat::R16Float;
        case TextureFormat::RG16Float:
            return wgpu::TextureFormat::RG16Float;
        case TextureFormat::RG32Float:
            return wgpu::TextureFormat::RG32Float;
        case TextureFormat::R16Unorm:
            return wgpu::TextureFormat::R16Unorm;
        case TextureFormat::R32Uint:
            return wgpu::TextureFormat::R32Uint;
        case TextureFormat::RGBA8UnormSrgb:
            return wgpu::TextureFormat::RGBA8UnormSrgb;
        case TextureFormat::BGRA8UnormSrgb:
            return wgpu::TextureFormat::BGRA8UnormSrgb;
        case TextureFormat::RG16Unorm:
            return wgpu::TextureFormat::RG16Unorm;
        case TextureFormat::RGBA16Unorm:
            return wgpu::TextureFormat::RGBA16Unorm;
        case TextureFormat::RGB10A2Unorm:
            return wgpu::TextureFormat::RGB10A2Unorm;
        case TextureFormat::RG11B10Float:
            return wgpu::TextureFormat::RG11B10Ufloat;
        case TextureFormat::RGB9E5Float:
            return wgpu::TextureFormat::RGB9E5Ufloat;
        case TextureFormat::R8Uint:
            return wgpu::TextureFormat::R8Uint;
        case TextureFormat::RG8Uint:
            return wgpu::TextureFormat::RG8Uint;
        case TextureFormat::RGBA8Uint:
            return wgpu::TextureFormat::RGBA8Uint;
        case TextureFormat::R16Uint:
            return wgpu::TextureFormat::R16Uint;
        case TextureFormat::RG16Uint:
            return wgpu::TextureFormat::RG16Uint;
        case TextureFormat::RGBA16Uint:
            return wgpu::TextureFormat::RGBA16Uint;
        case TextureFormat::RG32Uint:
            return wgpu::TextureFormat::RG32Uint;
        case TextureFormat::RGBA32Uint:
            return wgpu::TextureFormat::RGBA32Uint;
        case TextureFormat::R8Sint:
            return wgpu::TextureFormat::R8Sint;
        case TextureFormat::RG8Sint:
            return wgpu::TextureFormat::RG8Sint;
        case TextureFormat::RGBA8Sint:
            return wgpu::TextureFormat::RGBA8Sint;
        case TextureFormat::R16Sint:
            return wgpu::TextureFormat::R16Sint;
        case TextureFormat::RG16Sint:
            return wgpu::TextureFormat::RG16Sint;
        case TextureFormat::RGBA16Sint:
            return wgpu::TextureFormat::RGBA16Sint;
        case TextureFormat::R32Sint:
            return wgpu::TextureFormat::R32Sint;
        case TextureFormat::RG32Sint:
            return wgpu::TextureFormat::RG32Sint;
        case TextureFormat::RGBA32Sint:
            return wgpu::TextureFormat::RGBA32Sint;
        case TextureFormat::BC1RGBA:
            return wgpu::TextureFormat::BC1RGBAUnorm;
        case TextureFormat::BC2RGBA:
            return wgpu::TextureFormat::BC2RGBAUnorm;
        case TextureFormat::BC3RGBA:
            return wgpu::TextureFormat::BC3RGBAUnorm;
        case TextureFormat::BC7RGBA:
            return wgpu::TextureFormat::BC7RGBAUnorm;
        case TextureFormat::BC1RGBASrgb:
            return wgpu::TextureFormat::BC1RGBAUnormSrgb;
        case TextureFormat::BC2RGBASrgb:
            return wgpu::TextureFormat::BC2RGBAUnormSrgb;
        case TextureFormat::BC3RGBASrgb:
            return wgpu::TextureFormat::BC3RGBAUnormSrgb;
        case TextureFormat::BC7RGBASrgb:
            return wgpu::TextureFormat::BC7RGBAUnormSrgb;
        case TextureFormat::BC4RUnorm:
            return wgpu::TextureFormat::BC4RUnorm;
        case TextureFormat::BC4RSnorm:
            return wgpu::TextureFormat::BC4RSnorm;
        case TextureFormat::BC5RGUnorm:
            return wgpu::TextureFormat::BC5RGUnorm;
        case TextureFormat::BC5RGSnorm:
            return wgpu::TextureFormat::BC5RGSnorm;
        case TextureFormat::BC6HRGBUfloat:
            return wgpu::TextureFormat::BC6HRGBUfloat;
    }

    return wgpu::TextureFormat::Undefined;
}

wgpu::TextureFormat toWGPUFormat(PixelFormat format)
{
    return toWGPUFormat(textureFormatFor(format));
}

wgpu::CompareFunction toWGPUCompare(CompareFunction compare)
{
    switch (compare)
    {
        case CompareFunction::Never:
            return wgpu::CompareFunction::Never;
        case CompareFunction::Less:
            return wgpu::CompareFunction::Less;
        case CompareFunction::LessEqual:
            return wgpu::CompareFunction::LessEqual;
        case CompareFunction::Equal:
            return wgpu::CompareFunction::Equal;
        case CompareFunction::NotEqual:
            return wgpu::CompareFunction::NotEqual;
        case CompareFunction::GreaterEqual:
            return wgpu::CompareFunction::GreaterEqual;
        case CompareFunction::Greater:
            return wgpu::CompareFunction::Greater;
        case CompareFunction::Always:
            return wgpu::CompareFunction::Always;
    }

    return wgpu::CompareFunction::Always;
}

wgpu::TextureViewDimension sampledDimension(TextureType kind)
{
    switch (kind)
    {
        case TextureType::Cube:
            return wgpu::TextureViewDimension::Cube;
        case TextureType::TwoDArray:
            return wgpu::TextureViewDimension::e2DArray;
        case TextureType::CubeArray:
            return wgpu::TextureViewDimension::CubeArray;
        case TextureType::ThreeD:
            return wgpu::TextureViewDimension::e3D;
        default:
            return wgpu::TextureViewDimension::e2D;
    }
}
} // namespace eacp::GPU
