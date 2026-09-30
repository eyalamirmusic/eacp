#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../WebGPU/WebGPUTypes.h"
#include "ShaderSource.h"

namespace eacp::GPU
{
namespace
{
// Compilation is synchronous to the caller and its diagnostics are not, so
// they are logged as they arrive, against the line they name.
void onCompilationInfo(WGPUCompilationInfoRequestStatus status,
                       const WGPUCompilationInfo* info,
                       void*,
                       void*)
{
    if (status == WGPUCompilationInfoRequestStatus_Success && info != nullptr)
    {
        for (auto index = std::size_t {0}; index < info->messageCount; ++index)
        {
            const auto& message = info->messages[index];

            if (message.type != WGPUCompilationMessageType_Error)
                continue;

            LOG("WebGPU: WGSL error at line ",
                message.lineNum,
                ":",
                message.linePos,
                ": ",
                fromWebString(message.message));
        }
    }
}

WGPUShaderModule compileWgsl(const std::string& source)
{
    auto wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = toWebString(source);

    auto descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &wgsl.chain;

    auto module =
        wgpuDeviceCreateShaderModule(getWebGPUShared().getDevice(), &descriptor);

    if (module == nullptr)
        return nullptr;

    auto info = WGPU_COMPILATION_INFO_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = onCompilationInfo;

    wgpuShaderModuleGetCompilationInfo(module, info);

    return module;
}
} // namespace

struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source)
    {
        if (!device.isValid() || source.source.empty())
            return;

        if (source.backend != ShaderBackend::WebGPU)
        {
            LOG("WebGPU: a shader source that is not WGSL; build it with "
                "ShaderSource::wgsl");
            return;
        }

        auto& shared = getWebGPUShared();

        pushWebErrorScope();
        program.module = compileWgsl(source.source);
        popWebErrorScope("the shader module");

        if (program.module == nullptr)
            return;

        program.bindings =
            reflectWgslBindings(source.source, source.isCompute(), source.bindings);
        program.groupLayout = makeBindGroupLayout(program.bindings, "eacp shader");

        if (program.groupLayout == nullptr)
            return;

        auto descriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        descriptor.bindGroupLayoutCount = 1;
        descriptor.bindGroupLayouts = &program.groupLayout;

        program.pipelineLayout =
            wgpuDeviceCreatePipelineLayout(shared.getDevice(), &descriptor);
    }

    ~Native()
    {
        if (program.pipelineLayout != nullptr)
            wgpuPipelineLayoutRelease(program.pipelineLayout);

        if (program.groupLayout != nullptr)
            wgpuBindGroupLayoutRelease(program.groupLayout);

        if (program.module != nullptr)
            wgpuShaderModuleRelease(program.module);
    }

    WebShaderProgram program;
};

ShaderLibrary::ShaderLibrary(Device& device, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , groupShape(source.threadGroup)
    , impl(device, source)
{
}

bool ShaderLibrary::isValid() const
{
    return impl->program.isValid();
}

void* ShaderLibrary::nativeLibrary() const
{
    return const_cast<WebShaderProgram*>(&impl->program);
}
} // namespace eacp::GPU
