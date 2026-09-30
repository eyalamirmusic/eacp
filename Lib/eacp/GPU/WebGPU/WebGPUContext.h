#pragma once

#include "../Texture/Texture.h"

#include <eacp/Core/Utils/Containers.h>

#include <webgpu/webgpu.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// WebGPUShared is one per process: the browser hands over one GPUDevice, and
// every GPU::Device draws through it. WebGPUContext is one per GPU::Device and
// holds what a Device owns on the other backends: its uniform ring and the
// count of its submissions. Not part of GPU.h; the Web section of GPU/README.md
// has the decisions.

namespace eacp::GPU
{
class Device;

WGPUStringView toWebString(std::string_view text);
std::string fromWebString(WGPUStringView text);

// A block of the uniform ring: bound at `offset` through a dynamic offset,
// `size` bytes long (the block rounded up to 16).
struct UniformRange
{
    bool isValid() const { return buffer != nullptr; }

    WGPUBuffer buffer = nullptr;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

// What stands in for a binding the shader declares and the caller never bound,
// WebGPU having no partially bound groups.
enum class WebPlaceholder
{
    Texture2D,
    TextureCube,
    Depth,
    StorageRGBA8,
    StorageRGBA16Float,
    StorageRGBA32Float
};

class WebGPUShared
{
public:
    WebGPUShared();
    ~WebGPUShared();

    WebGPUShared(const WebGPUShared&) = delete;
    WebGPUShared& operator=(const WebGPUShared&) = delete;

    bool isValid() const { return device != nullptr; }

    WGPUInstance getInstance() const { return instance; }
    WGPUDevice getDevice() const { return device; }
    WGPUQueue getQueue() const { return queue; }

    const WGPULimits& getLimits() const { return limits; }
    bool hasFeature(WGPUFeatureName feature) const;

    const std::string& getAdapterName() const { return adapterName; }

    // One per samplingIndex. A binding the layout calls non-filtering takes the
    // nearest sampler at the same address mode, a linear one being invalid there.
    WGPUSampler getSampler(TextureSampling sampling, bool filtering) const;

    WGPUTextureView getPlaceholder(WebPlaceholder kind);
    WGPUBuffer getPlaceholderBuffer();
    static constexpr std::uint64_t placeholderBufferBytes = 4096;

    // Zeros for a uniform block the shader declares and nothing set: bound at
    // offset zero with this size.
    WGPUBuffer getZeroUniforms();
    static constexpr std::uint32_t zeroUniformBytes = 16384;

private:
    void createAll();
    void createSamplers();

    WGPUInstance instance = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPULimits limits = WGPU_LIMITS_INIT;
    Vector<WGPUFeatureName> features;
    std::string adapterName = "no WebGPU device";

    WGPUSampler samplers[samplingConfigurations] = {};

    struct Placeholder
    {
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
    };

    Placeholder placeholders[6] = {};
    WGPUBuffer placeholderBuffer = nullptr;
    WGPUBuffer zeroUniforms = nullptr;
};

WebGPUShared& getWebGPUShared();

class WebGPUContext
{
public:
    WebGPUContext();
    ~WebGPUContext();

    WebGPUContext(const WebGPUContext&) = delete;
    WebGPUContext& operator=(const WebGPUContext&) = delete;

    bool isValid() const { return getWebGPUShared().isValid(); }

    WGPUDevice getDevice() const { return getWebGPUShared().getDevice(); }
    WGPUQueue getQueue() const { return getWebGPUShared().getQueue(); }

    // Null on an invalid device.
    WGPUCommandEncoder beginRecording();

    // Flushes the uniform ring, finishes and submits. Returns the submission's
    // value, zero if nothing was submitted.
    std::uint64_t submit(WGPUCommandEncoder encoder);

    // An encoder dropped without a submit.
    void discard(WGPUCommandEncoder encoder);

    // Staged on the CPU and written with one writeBuffer per page at the next
    // submit, which is before any command naming it can run.
    UniformRange uploadUniforms(const void* data, std::size_t bytes);

    std::uint64_t lastSubmitted() const { return submitted; }
    bool hasCompleted(std::uint64_t value) const;

    // Fires inline if `value` has passed, else when the queue reports it.
    void notifyWhenCompleted(std::uint64_t value, Callback done);

    void followMainThread() {}

private:
    struct Tracker
    {
        struct Pending
        {
            std::uint64_t value = 0;
            Callback done;
        };

        void complete(std::uint64_t value);

        std::uint64_t completed = 0;
        Vector<Pending> pending;
    };

    struct UniformPage
    {
        WGPUBuffer buffer = nullptr;
        Vector<std::byte> staging;
        std::size_t used = 0;
        std::size_t flushed = 0;
    };

    void flushUniforms();
    void rewindUniforms();
    UniformPage* pageFor(std::size_t bytes);

    std::shared_ptr<Tracker> tracker = std::make_shared<Tracker>();
    std::uint64_t submitted = 0;

    std::vector<std::unique_ptr<UniformPage>> uniformPages;
    int uniformCursor = 0;
    int openRecordings = 0;

    static constexpr std::size_t uniformPageBytes = 256 * 1024;
};

// A resource does not cross Devices.
WebGPUContext& getWebGPUContext(const Device& device);
} // namespace eacp::GPU
