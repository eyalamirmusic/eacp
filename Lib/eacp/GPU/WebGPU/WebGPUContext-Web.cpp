#include "WebGPUContext.h"

#include "../Device/Device.h"
#include "WebGPUTypes.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstring>

namespace eacp::GPU
{
namespace
{
EM_JS(bool, webHasPreinitializedDevice, (), {
    return !!Module['preinitializedWebGPUDevice'];
});

// Errors outside a scope would otherwise surface only as the browser's own
// console warning, and a lost device not at all.
EM_JS(void, webReportDeviceErrorsToConsole, (), {
    var device = Module['preinitializedWebGPUDevice'];

    if (!device || device.eacpReportsErrors)
        return;

    device.eacpReportsErrors = true;
    device.addEventListener(
        'uncapturederror',
        function(event) { console.error('eacp WebGPU: ' + event.error.message); });
    device.lost.then(function(info) {
        console.error('eacp WebGPU: the device was lost: ' + info.message);
    });
});

std::size_t roundUpTo(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

WGPUAddressMode toWebAddressMode(TextureAddressMode mode)
{
    return mode == TextureAddressMode::Repeat ? WGPUAddressMode_Repeat
                                              : WGPUAddressMode_ClampToEdge;
}

WGPUSampler makeSampler(WGPUDevice device, TextureSampling sampling)
{
    const auto linear = sampling.filter == TextureFilter::Linear;
    const auto address = toWebAddressMode(sampling.addressMode);

    auto descriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
    descriptor.addressModeU = address;
    descriptor.addressModeV = address;
    descriptor.addressModeW = address;
    descriptor.magFilter = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    descriptor.minFilter = descriptor.magFilter;
    descriptor.mipmapFilter =
        linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;

    return wgpuDeviceCreateSampler(device, &descriptor);
}

struct PlaceholderShape
{
    WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
    WGPUTextureUsage usage = WGPUTextureUsage_TextureBinding;
    WGPUTextureViewDimension dimension = WGPUTextureViewDimension_2D;
    int layers = 1;
};

PlaceholderShape placeholderShape(WebPlaceholder kind)
{
    auto shape = PlaceholderShape {};

    switch (kind)
    {
        case WebPlaceholder::Texture2D:
            break;

        case WebPlaceholder::TextureCube:
            shape.dimension = WGPUTextureViewDimension_Cube;
            shape.layers = 6;
            break;

        case WebPlaceholder::Depth:
            shape.format = WGPUTextureFormat_Depth32Float;
            break;

        case WebPlaceholder::StorageRGBA8:
            shape.usage = WGPUTextureUsage_StorageBinding;
            break;

        case WebPlaceholder::StorageRGBA16Float:
            shape.format = WGPUTextureFormat_RGBA16Float;
            shape.usage = WGPUTextureUsage_StorageBinding;
            break;

        case WebPlaceholder::StorageRGBA32Float:
            shape.format = WGPUTextureFormat_RGBA32Float;
            shape.usage = WGPUTextureUsage_StorageBinding;
            break;
    }

    return shape;
}

struct WorkDone
{
    std::weak_ptr<void> tracker;
    std::uint64_t value = 0;
    void (*complete)(void* tracker, std::uint64_t value) = nullptr;
};

void onWorkDone(WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata, void*)
{
    auto* done = static_cast<WorkDone*>(userdata);

    if (auto tracker = done->tracker.lock())
        done->complete(tracker.get(), done->value);

    delete done;
}

void onErrorScope(WGPUPopErrorScopeStatus status,
                  WGPUErrorType type,
                  WGPUStringView message,
                  void* userdata,
                  void*)
{
    auto* label = static_cast<std::string*>(userdata);

    if (status == WGPUPopErrorScopeStatus_Success && type != WGPUErrorType_NoError)
        LOG("WebGPU: ", *label, " failed: ", fromWebString(message));

    delete label;
}
} // namespace

WGPUStringView toWebString(std::string_view text)
{
    return {text.data(), text.size()};
}

std::string fromWebString(WGPUStringView text)
{
    if (text.data == nullptr)
        return {};

    if (text.length == WGPU_STRLEN)
        return text.data;

    return {text.data, text.length};
}

void reportWebUnsupported(const char* what)
{
    static auto reported = Vector<const char*> {};

    for (auto* seen: reported)
        if (std::strcmp(seen, what) == 0)
            return;

    reported.add(what);
    LOG("WebGPU: ",
        what,
        " is unsupported on the web without JSPI - nothing may block the "
        "browser's main thread");
}

void pushWebErrorScope()
{
    auto& shared = getWebGPUShared();

    if (shared.isValid())
        wgpuDevicePushErrorScope(shared.getDevice(), WGPUErrorFilter_Validation);
}

void popWebErrorScope(std::string label)
{
    auto& shared = getWebGPUShared();

    if (!shared.isValid())
        return;

    auto info = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = onErrorScope;
    info.userdata1 = new std::string(std::move(label));

    wgpuDevicePopErrorScope(shared.getDevice(), info);
}

WebGPUShared::WebGPUShared()
{
    createAll();
}

WebGPUShared::~WebGPUShared()
{
    for (auto sampler: samplers)
        if (sampler != nullptr)
            wgpuSamplerRelease(sampler);

    for (auto& placeholder: placeholders)
    {
        if (placeholder.view != nullptr)
            wgpuTextureViewRelease(placeholder.view);

        if (placeholder.texture != nullptr)
            wgpuTextureRelease(placeholder.texture);
    }

    for (auto buffer: {placeholderBuffer, zeroUniforms})
        if (buffer != nullptr)
            wgpuBufferRelease(buffer);

    if (queue != nullptr)
        wgpuQueueRelease(queue);

    if (device != nullptr)
        wgpuDeviceRelease(device);

    if (instance != nullptr)
        wgpuInstanceRelease(instance);
}

// The device arrives from the page: the shell requests it before main runs and
// leaves it on Module.preinitializedWebGPUDevice, which is how a synchronous
// constructor gets one where requesting is a promise.
void WebGPUShared::createAll()
{
    if (!webHasPreinitializedDevice())
    {
        LOG("WebGPU: the page handed over no device (Module."
            "preinitializedWebGPUDevice), so this browser has no WebGPU or the "
            "shell did not request one; the GPU is unavailable");
        return;
    }

    instance = wgpuCreateInstance(nullptr);
    device = emscripten_webgpu_get_device();

    if (device == nullptr)
        return;

    queue = wgpuDeviceGetQueue(device);

    wgpuDeviceGetLimits(device, &limits);

    auto supported = WGPUSupportedFeatures {};
    wgpuDeviceGetFeatures(device, &supported);

    for (auto index = std::size_t {0}; index < supported.featureCount; ++index)
        features.add(supported.features[index]);

    wgpuSupportedFeaturesFreeMembers(supported);

    auto info = WGPU_ADAPTER_INFO_INIT;

    if (wgpuDeviceGetAdapterInfo(device, &info) == WGPUStatus_Success)
    {
        auto name = fromWebString(info.description);

        if (name.empty())
            name =
                fromWebString(info.vendor) + " " + fromWebString(info.architecture);

        adapterName = "WebGPU: " + name;
        wgpuAdapterInfoFreeMembers(info);
    }

    createSamplers();
    webReportDeviceErrorsToConsole();
}

void WebGPUShared::createSamplers()
{
    for (auto linear: {false, true})
        for (auto repeat: {false, true})
        {
            auto sampling = TextureSampling {};
            sampling.filter =
                linear ? TextureFilter::Linear : TextureFilter::Nearest;
            sampling.addressMode =
                repeat ? TextureAddressMode::Repeat : TextureAddressMode::Clamp;

            samplers[samplingIndex(sampling)] = makeSampler(device, sampling);
        }
}

bool WebGPUShared::hasFeature(WGPUFeatureName feature) const
{
    return features.contains(feature);
}

WGPUSampler WebGPUShared::getSampler(TextureSampling sampling, bool filtering) const
{
    if (!filtering)
        sampling.filter = TextureFilter::Nearest;

    return samplers[samplingIndex(sampling)];
}

WGPUTextureView WebGPUShared::getPlaceholder(WebPlaceholder kind)
{
    auto& placeholder = placeholders[static_cast<int>(kind)];

    if (placeholder.view != nullptr || device == nullptr)
        return placeholder.view;

    const auto shape = placeholderShape(kind);

    auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.label = toWebString("eacp placeholder");
    descriptor.usage = shape.usage;
    descriptor.size = {1, 1, static_cast<std::uint32_t>(shape.layers)};
    descriptor.format = shape.format;

    placeholder.texture = wgpuDeviceCreateTexture(device, &descriptor);

    auto view = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    view.dimension = shape.dimension;

    placeholder.view = wgpuTextureCreateView(placeholder.texture, &view);

    return placeholder.view;
}

WGPUBuffer WebGPUShared::getPlaceholderBuffer()
{
    if (placeholderBuffer != nullptr || device == nullptr)
        return placeholderBuffer;

    auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.label = toWebString("eacp placeholder");
    descriptor.usage = WGPUBufferUsage_Storage;
    descriptor.size = placeholderBufferBytes;

    placeholderBuffer = wgpuDeviceCreateBuffer(device, &descriptor);

    return placeholderBuffer;
}

WGPUBuffer WebGPUShared::getZeroUniforms()
{
    if (zeroUniforms != nullptr || device == nullptr)
        return zeroUniforms;

    auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.label = toWebString("eacp zero uniforms");
    descriptor.usage = WGPUBufferUsage_Uniform;
    descriptor.size = zeroUniformBytes;

    zeroUniforms = wgpuDeviceCreateBuffer(device, &descriptor);

    return zeroUniforms;
}

WebGPUShared& getWebGPUShared()
{
    static WebGPUShared shared;
    return shared;
}

void WebGPUContext::Tracker::complete(std::uint64_t value)
{
    completed = std::max(completed, value);

    auto ready = Vector<Callback> {};

    for (auto index = pending.size() - 1; index >= 0; --index)
    {
        if (pending[index].value > completed)
            continue;

        ready.add(std::move(pending[index].done));
        pending.removeAt(index);
    }

    for (auto& done: ready)
        done();
}

WebGPUContext::WebGPUContext() = default;

WebGPUContext::~WebGPUContext()
{
    for (auto& page: uniformPages)
        if (page->buffer != nullptr)
            wgpuBufferRelease(page->buffer);
}

WGPUCommandEncoder WebGPUContext::beginRecording()
{
    if (!isValid())
        return nullptr;

    auto descriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    auto encoder = wgpuDeviceCreateCommandEncoder(getDevice(), &descriptor);

    if (encoder != nullptr)
        ++openRecordings;

    return encoder;
}

std::uint64_t WebGPUContext::submit(WGPUCommandEncoder encoder)
{
    if (encoder == nullptr)
        return 0;

    --openRecordings;

    auto descriptor = WGPUCommandBufferDescriptor {};
    auto commands = wgpuCommandEncoderFinish(encoder, &descriptor);
    wgpuCommandEncoderRelease(encoder);

    flushUniforms();

    if (commands == nullptr)
        return 0;

    wgpuQueueSubmit(getQueue(), 1, &commands);
    wgpuCommandBufferRelease(commands);

    const auto value = ++submitted;

    auto* done = new WorkDone;
    done->tracker = std::static_pointer_cast<void>(tracker);
    done->value = value;
    done->complete = [](void* owner, std::uint64_t completedValue)
    { static_cast<Tracker*>(owner)->complete(completedValue); };

    auto info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = onWorkDone;
    info.userdata1 = done;

    wgpuQueueOnSubmittedWorkDone(getQueue(), info);

    // With nothing else recording, no command still to be submitted names the
    // ring, and a write from here on lands behind everything that read it.
    if (openRecordings == 0)
        rewindUniforms();

    return value;
}

void WebGPUContext::discard(WGPUCommandEncoder encoder)
{
    if (encoder == nullptr)
        return;

    --openRecordings;
    wgpuCommandEncoderRelease(encoder);

    if (openRecordings == 0)
    {
        flushUniforms();
        rewindUniforms();
    }
}

bool WebGPUContext::hasCompleted(std::uint64_t value) const
{
    return value <= tracker->completed;
}

void WebGPUContext::notifyWhenCompleted(std::uint64_t value, Callback done)
{
    if (hasCompleted(value))
    {
        done();
        return;
    }

    tracker->pending.add({value, std::move(done)});
}

UniformRange WebGPUContext::uploadUniforms(const void* data, std::size_t bytes)
{
    if (!isValid() || data == nullptr || bytes == 0)
        return {};

    auto* page = pageFor(bytes);

    if (page == nullptr)
        return {};

    const auto alignment = std::max<std::size_t>(
        getWebGPUShared().getLimits().minUniformBufferOffsetAlignment, 16);

    const auto offset = page->used;
    std::memcpy(page->staging.data() + offset, data, bytes);

    const auto size = roundUpTo(bytes, 16);
    std::memset(page->staging.data() + offset + bytes, 0, size - bytes);

    page->used = roundUpTo(offset + size, alignment);

    auto range = UniformRange {};
    range.buffer = page->buffer;
    range.offset = static_cast<std::uint32_t>(offset);
    range.size = static_cast<std::uint32_t>(size);

    return range;
}

WebGPUContext::UniformPage* WebGPUContext::pageFor(std::size_t bytes)
{
    const auto needed = roundUpTo(bytes, 16);

    if (needed > uniformPageBytes)
        return nullptr;

    while (uniformCursor < static_cast<int>(uniformPages.size()))
    {
        auto& page = *uniformPages[static_cast<std::size_t>(uniformCursor)];

        if (page.used + needed <= uniformPageBytes)
            return &page;

        ++uniformCursor;
    }

    auto page = std::make_unique<UniformPage>();

    auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.label = toWebString("eacp uniforms");
    descriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    descriptor.size = uniformPageBytes;

    page->buffer = wgpuDeviceCreateBuffer(getDevice(), &descriptor);

    if (page->buffer == nullptr)
        return nullptr;

    page->staging.resize(static_cast<int>(uniformPageBytes));

    uniformPages.push_back(std::move(page));
    uniformCursor = static_cast<int>(uniformPages.size()) - 1;

    return uniformPages.back().get();
}

void WebGPUContext::flushUniforms()
{
    for (auto& page: uniformPages)
    {
        if (page->used <= page->flushed)
            continue;

        wgpuQueueWriteBuffer(getQueue(),
                             page->buffer,
                             page->flushed,
                             page->staging.data() + page->flushed,
                             page->used - page->flushed);

        page->flushed = page->used;
    }
}

void WebGPUContext::rewindUniforms()
{
    for (auto& page: uniformPages)
    {
        page->used = 0;
        page->flushed = 0;
    }

    uniformCursor = 0;
}

WebGPUContext& getWebGPUContext(const Device& device)
{
    return *static_cast<WebGPUContext*>(device.nativeContext());
}
} // namespace eacp::GPU
