#include "WebGPUTypes.h"

#include "../Buffer/Buffer.h"

#include <cctype>
#include <string_view>

namespace eacp::GPU
{
namespace
{
struct Declaration
{
    int group = 0;
    int binding = -1;
    std::string addressSpace;
    std::string type;
};

int parseNumberAfter(std::string_view text, std::size_t& position)
{
    while (position < text.size() && !std::isdigit((unsigned char) text[position]))
        ++position;

    auto value = 0;

    while (position < text.size() && std::isdigit((unsigned char) text[position]))
        value = value * 10 + (text[position++] - '0');

    return value;
}

std::string trimmed(std::string_view text)
{
    auto first = std::size_t {0};
    auto last = text.size();

    while (first < last && std::isspace((unsigned char) text[first]))
        ++first;

    while (last > first && std::isspace((unsigned char) text[last - 1]))
        --last;

    return std::string(text.substr(first, last - first));
}

// Each `@group(g) @binding(b) var<space> name : type;` in the source, in order.
Vector<Declaration> findDeclarations(std::string_view source)
{
    auto declarations = Vector<Declaration> {};
    auto position = std::size_t {0};

    while ((position = source.find("@binding(", position)) != std::string_view::npos)
    {
        auto declaration = Declaration {};

        const auto groupAt = source.rfind("@group(", position);
        const auto lineStart = source.rfind('\n', position);

        if (groupAt != std::string_view::npos
            && (lineStart == std::string_view::npos || groupAt > lineStart))
        {
            auto cursor = groupAt;
            declaration.group = parseNumberAfter(source, cursor);
        }

        declaration.binding = parseNumberAfter(source, position);

        const auto varAt = source.find("var", position);
        const auto end = source.find(';', position);

        if (varAt == std::string_view::npos || end == std::string_view::npos
            || varAt > end)
            continue;

        auto cursor = varAt + 3;

        if (cursor < source.size() && source[cursor] == '<')
        {
            const auto close = source.find('>', cursor);

            if (close == std::string_view::npos || close > end)
                continue;

            declaration.addressSpace =
                trimmed(source.substr(cursor + 1, close - cursor - 1));
            cursor = close + 1;
        }

        const auto colon = source.find(':', cursor);

        if (colon == std::string_view::npos || colon > end)
            continue;

        declaration.type = trimmed(source.substr(colon + 1, end - colon - 1));
        declarations.add(declaration);
        position = end;
    }

    return declarations;
}

bool startsWith(const std::string& text, std::string_view prefix)
{
    return text.compare(0, prefix.size(), prefix) == 0;
}

WGPUTextureFormat storageFormatOf(const std::string& type)
{
    const auto open = type.find('<');
    const auto comma = type.find(',');

    if (open == std::string::npos || comma == std::string::npos || comma < open)
        return WGPUTextureFormat_Undefined;

    const auto format =
        trimmed(std::string_view(type).substr(open + 1, comma - open - 1));

    if (format == "rgba8unorm")
        return WGPUTextureFormat_RGBA8Unorm;

    if (format == "rgba16float")
        return WGPUTextureFormat_RGBA16Float;

    if (format == "rgba32float")
        return WGPUTextureFormat_RGBA32Float;

    if (format == "r32float")
        return WGPUTextureFormat_R32Float;

    return WGPUTextureFormat_Undefined;
}

bool classify(const Declaration& declaration, WebBindingKind& kind)
{
    const auto& type = declaration.type;
    const auto& space = declaration.addressSpace;

    if (startsWith(space, "uniform"))
        kind = WebBindingKind::Uniform;
    else if (startsWith(space, "storage"))
        kind = space.find("read_write") != std::string::npos
                   ? WebBindingKind::StorageBuffer
                   : WebBindingKind::ReadOnlyStorageBuffer;
    else if (startsWith(type, "texture_storage_2d"))
        kind = WebBindingKind::StorageTexture;
    else if (startsWith(type, "texture_depth_2d"))
        kind = WebBindingKind::DepthTexture;
    else if (startsWith(type, "texture_cube"))
        kind = WebBindingKind::TextureCube;
    else if (startsWith(type, "texture_2d"))
        kind = WebBindingKind::Texture;
    else if (startsWith(type, "sampler"))
        kind = WebBindingKind::Sampler;
    else
        return false;

    return true;
}

bool isTextureKind(WebBindingKind kind)
{
    return kind == WebBindingKind::Texture || kind == WebBindingKind::TextureCube
           || kind == WebBindingKind::DepthTexture
           || kind == WebBindingKind::StorageTexture;
}

int slotOf(int binding, WebBindingKind kind, bool compute)
{
    if (kind == WebBindingKind::Uniform)
        return 0;

    if (kind == WebBindingKind::Sampler)
        return binding - wgslSamplerBase;

    if (isTextureKind(kind))
        return binding
               - (compute ? wgslComputeTextureBinding(0) : wgslTextureBinding(0));

    return binding - (compute ? wgslComputeBufferBinding(0) : wgslBufferBinding(0));
}

// Linear where the shader said so, and where nothing said anything - a
// hand-written source - since that is what an unannotated float texture is.
bool declaredFiltering(const Vector<ResourceBinding>& declared, int slot)
{
    for (const auto& binding: declared)
        if (binding.kind == ResourceKind::Sampler && binding.index == slot)
            return binding.sampling.filter == TextureFilter::Linear;

    return true;
}

WGPUShaderStage visibilityOf(const WebBinding& binding, bool compute)
{
    if (compute)
        return WGPUShaderStage_Compute;

    if (binding.kind == WebBindingKind::StorageTexture
        || binding.kind == WebBindingKind::StorageBuffer)
        return WGPUShaderStage_Fragment;

    return WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
}

WGPUTextureSampleType sampleTypeOf(const WebBinding& binding)
{
    if (binding.kind == WebBindingKind::DepthTexture)
        return WGPUTextureSampleType_Depth;

    return binding.filtering ? WGPUTextureSampleType_Float
                             : WGPUTextureSampleType_UnfilterableFloat;
}

WebPlaceholder placeholderFor(const WebBinding& binding)
{
    switch (binding.kind)
    {
        case WebBindingKind::TextureCube:
            return WebPlaceholder::TextureCube;

        case WebBindingKind::DepthTexture:
            return WebPlaceholder::Depth;

        case WebBindingKind::StorageTexture:
            if (binding.storageFormat == WGPUTextureFormat_RGBA16Float)
                return WebPlaceholder::StorageRGBA16Float;

            if (binding.storageFormat == WGPUTextureFormat_RGBA32Float)
                return WebPlaceholder::StorageRGBA32Float;

            return WebPlaceholder::StorageRGBA8;

        case WebBindingKind::Uniform:
        case WebBindingKind::Texture:
        case WebBindingKind::Sampler:
        case WebBindingKind::StorageBuffer:
        case WebBindingKind::ReadOnlyStorageBuffer:
            break;
    }

    return WebPlaceholder::Texture2D;
}

struct StageCounts
{
    std::uint32_t sampledTextures = 0;
    std::uint32_t samplers = 0;
    std::uint32_t storageBuffers = 0;
    std::uint32_t storageTextures = 0;
    std::uint32_t uniforms = 0;
};

bool fitsLimits(const WebBindingLayout& layout, std::string_view label)
{
    auto counts = StageCounts {};

    for (const auto& binding: layout.entries)
    {
        switch (binding.kind)
        {
            case WebBindingKind::Uniform:
                ++counts.uniforms;
                break;

            case WebBindingKind::Texture:
            case WebBindingKind::TextureCube:
            case WebBindingKind::DepthTexture:
                ++counts.sampledTextures;
                break;

            case WebBindingKind::StorageTexture:
                ++counts.storageTextures;
                break;

            case WebBindingKind::Sampler:
                ++counts.samplers;
                break;

            case WebBindingKind::StorageBuffer:
            case WebBindingKind::ReadOnlyStorageBuffer:
                ++counts.storageBuffers;
                break;
        }
    }

    const auto& limits = getWebGPUShared().getLimits();

    const auto check = [&](std::uint32_t used, std::uint32_t limit, const char* what)
    {
        if (used <= limit)
            return true;

        LOG("WebGPU: ",
            label,
            " declares ",
            used,
            " ",
            what,
            " in one stage; this device allows ",
            limit);

        return false;
    };

    return check(counts.sampledTextures,
                 limits.maxSampledTexturesPerShaderStage,
                 "sampled textures")
           && check(counts.samplers, limits.maxSamplersPerShaderStage, "samplers")
           && check(counts.storageBuffers,
                    limits.maxStorageBuffersPerShaderStage,
                    "storage buffers")
           && check(counts.storageTextures,
                    limits.maxStorageTexturesPerShaderStage,
                    "storage textures")
           && check(counts.uniforms,
                    limits.maxDynamicUniformBuffersPerPipelineLayout,
                    "dynamic uniform blocks");
}
} // namespace

const WebBinding* WebBindingLayout::find(WebBindingKind kind, int slot) const
{
    for (const auto& binding: entries)
        if (binding.kind == kind && binding.slot == slot)
            return &binding;

    return nullptr;
}

bool WebBindingLayout::hasUniforms() const
{
    return find(WebBindingKind::Uniform, 0) != nullptr;
}

WebBindingLayout reflectWgslBindings(const std::string& wgsl,
                                     bool compute,
                                     const Vector<ResourceBinding>& declared)
{
    auto layout = WebBindingLayout {};
    layout.compute = compute;

    for (const auto& declaration: findDeclarations(wgsl))
    {
        auto kind = WebBindingKind::Texture;

        if (declaration.group != 0 || !classify(declaration, kind))
        {
            LOG("WebGPU: a WGSL binding eacp does not bind (group ",
                declaration.group,
                ", binding ",
                declaration.binding,
                ": ",
                declaration.type,
                ") - the pipeline will not validate");
            continue;
        }

        auto binding = WebBinding {};
        binding.binding = declaration.binding;
        binding.kind = kind;
        binding.slot = slotOf(declaration.binding, kind, compute);

        if (kind == WebBindingKind::StorageTexture)
            binding.storageFormat = storageFormatOf(declaration.type);

        layout.entries.add(binding);
    }

    // A depth texture's sampler never filters: whether a filtering one may
    // sample depth is not something every implementation agrees on.
    for (auto& binding: layout.entries)
    {
        if (binding.kind == WebBindingKind::Texture
            || binding.kind == WebBindingKind::TextureCube)
            binding.filtering = declaredFiltering(declared, binding.slot);

        if (binding.kind == WebBindingKind::Sampler)
        {
            const auto* depth =
                layout.find(WebBindingKind::DepthTexture, binding.slot);

            binding.filtering =
                depth == nullptr && declaredFiltering(declared, binding.slot);
        }
    }

    return layout;
}

WGPUBindGroupLayout makeBindGroupLayout(const WebBindingLayout& layout,
                                        std::string_view label)
{
    auto& shared = getWebGPUShared();

    if (!shared.isValid() || !fitsLimits(layout, label))
        return nullptr;

    auto entries = Vector<WGPUBindGroupLayoutEntry> {};

    for (const auto& binding: layout.entries)
    {
        auto entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        entry.binding = static_cast<std::uint32_t>(binding.binding);
        entry.visibility = visibilityOf(binding, layout.compute);

        switch (binding.kind)
        {
            case WebBindingKind::Uniform:
                entry.buffer.type = WGPUBufferBindingType_Uniform;
                entry.buffer.hasDynamicOffset = true;
                break;

            case WebBindingKind::StorageBuffer:
                entry.buffer.type = WGPUBufferBindingType_Storage;
                break;

            case WebBindingKind::ReadOnlyStorageBuffer:
                entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
                break;

            case WebBindingKind::Texture:
            case WebBindingKind::DepthTexture:
                entry.texture.sampleType = sampleTypeOf(binding);
                entry.texture.viewDimension = WGPUTextureViewDimension_2D;
                break;

            case WebBindingKind::TextureCube:
                entry.texture.sampleType = sampleTypeOf(binding);
                entry.texture.viewDimension = WGPUTextureViewDimension_Cube;
                break;

            case WebBindingKind::StorageTexture:
                entry.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
                entry.storageTexture.format = binding.storageFormat;
                entry.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
                break;

            case WebBindingKind::Sampler:
                entry.sampler.type = binding.filtering
                                         ? WGPUSamplerBindingType_Filtering
                                         : WGPUSamplerBindingType_NonFiltering;
                break;
        }

        entries.add(entry);
    }

    auto descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    descriptor.label = toWebString(label);
    descriptor.entryCount = static_cast<std::size_t>(entries.size());
    descriptor.entries = entries.data();

    return wgpuDeviceCreateBindGroupLayout(shared.getDevice(), &descriptor);
}

WebBufferBind webStorageBufferBind(const BufferRange& range)
{
    if (range.buffer == nullptr || range.offset < 0)
        return {};

    auto* data = static_cast<WebBufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->buffer == nullptr
        || static_cast<std::size_t>(range.offset) >= data->size)
        return {};

    const auto alignment =
        getWebGPUShared().getLimits().minStorageBufferOffsetAlignment;

    const auto offset = static_cast<std::uint64_t>(range.offset);

    if (alignment > 0 && offset % alignment != 0)
        return {};

    const auto size =
        (static_cast<std::uint64_t>(data->allocated) - offset) & ~std::uint64_t {3};

    if (size == 0)
        return {};

    return {data->buffer, offset, size};
}

WGPUBindGroup makeWebBindGroup(const WebBindingLayout& layout,
                               WGPUBindGroupLayout groupLayout,
                               const WebBoundResources& bound)
{
    auto& shared = getWebGPUShared();

    if (!shared.isValid() || groupLayout == nullptr)
        return nullptr;

    auto entries = Vector<WGPUBindGroupEntry> {};

    for (const auto& binding: layout.entries)
    {
        auto entry = WGPU_BIND_GROUP_ENTRY_INIT;
        entry.binding = static_cast<std::uint32_t>(binding.binding);

        const auto inTextureRange =
            binding.slot >= 0 && binding.slot < maxTextureSlots;
        const auto inBufferRange =
            binding.slot >= 0 && binding.slot < webMaxBufferSlots;

        switch (binding.kind)
        {
            case WebBindingKind::Uniform:
                if (bound.uniforms.isValid())
                {
                    entry.buffer = bound.uniforms.buffer;
                    entry.size = bound.uniforms.size;
                }
                else
                {
                    entry.buffer = shared.getZeroUniforms();
                    entry.size = WebGPUShared::zeroUniformBytes;
                }
                break;

            case WebBindingKind::StorageBuffer:
            case WebBindingKind::ReadOnlyStorageBuffer:
                if (inBufferRange && bound.buffers[binding.slot].isValid())
                {
                    const auto& buffer = bound.buffers[binding.slot];
                    entry.buffer = buffer.buffer;
                    entry.offset = buffer.offset;
                    entry.size = buffer.size;
                }
                else
                {
                    entry.buffer = shared.getPlaceholderBuffer();
                    entry.size = WebGPUShared::placeholderBufferBytes;
                }
                break;

            case WebBindingKind::Texture:
            case WebBindingKind::TextureCube:
            case WebBindingKind::DepthTexture:
            case WebBindingKind::StorageTexture:
                entry.textureView =
                    inTextureRange ? bound.textures[binding.slot].view : nullptr;

                if (entry.textureView == nullptr)
                    entry.textureView =
                        shared.getPlaceholder(placeholderFor(binding));
                break;

            case WebBindingKind::Sampler:
                entry.sampler = shared.getSampler(
                    inTextureRange ? bound.textures[binding.slot].sampling
                                   : TextureSampling {},
                    binding.filtering);
                break;
        }

        entries.add(entry);
    }

    auto descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.layout = groupLayout;
    descriptor.entryCount = static_cast<std::size_t>(entries.size());
    descriptor.entries = entries.data();

    return wgpuDeviceCreateBindGroup(shared.getDevice(), &descriptor);
}

std::uint32_t webUniformOffset(const WebBindingLayout&,
                               const WebBoundResources& bound)
{
    return bound.uniforms.isValid() ? bound.uniforms.offset : 0;
}
} // namespace eacp::GPU
