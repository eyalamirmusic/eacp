#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../WebGPU/WebGPUContext.h"

#include <eacp/Core/Utils/Logging.h>

#include <string>
#include <string_view>
#include <vector>

namespace eacp::GPU
{
namespace
{
CompilationMessageType messageTypeOf(wgpu::CompilationMessageType type)
{
    switch (type)
    {
        case wgpu::CompilationMessageType::Warning:
            return CompilationMessageType::Warning;
        case wgpu::CompilationMessageType::Info:
            return CompilationMessageType::Info;
        default:
            return CompilationMessageType::Error;
    }
}

// The messages Chrome's WGSL compiler gave, where this executable can wait for
// them; nothing where it cannot, and the pipeline is then the only check.
CompilationInfo collectMessages(const wgpu::ShaderModule& module)
{
    auto info = CompilationInfo {};

    if (!webgpuCanWait())
        return info;

    auto done = false;

    module.GetCompilationInfo(
        wgpu::CallbackMode::AllowSpontaneous,
        [&](wgpu::CompilationInfoRequestStatus status,
            const wgpu::CompilationInfo* result)
        {
            if (status == wgpu::CompilationInfoRequestStatus::Success
                && result != nullptr)
            {
                for (auto i = std::size_t {0}; i < result->messageCount; ++i)
                {
                    const auto& from = result->messages[i];
                    auto message = CompilationMessage {};
                    message.type = messageTypeOf(from.type);
                    message.message =
                        std::string(from.message.data, from.message.length);
                    message.line = static_cast<int>(from.lineNum);
                    message.column = static_cast<int>(from.linePos);
                    message.length = static_cast<int>(from.length);
                    info.messages.add(message);
                }
            }

            webgpuWake(done);
        });

    webgpuWaitUntil(done);
    return info;
}

// The entry points a pipeline will ask the module for. An empty module is
// valid WGSL, so a library is only whole once the source declares them.
std::vector<std::string> entriesOf(const ShaderSource& source)
{
    if (source.isCompute())
        return {source.computeEntry};

    auto entries = std::vector<std::string> {source.vertexEntry};

    if (!source.fragmentEntry.empty())
        entries.push_back(source.fragmentEntry);

    return entries;
}

bool declaresEntry(std::string_view wgsl, const std::string& entry)
{
    const auto declaration = "fn " + entry;

    for (auto at = wgsl.find(declaration); at != std::string_view::npos;
         at = wgsl.find(declaration, at + 1))
    {
        const auto next = at + declaration.size();

        if (next < wgsl.size() && (wgsl[next] == '(' || wgsl[next] == ' '))
            return true;
    }

    return false;
}
} // namespace

struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source, CompilationInfo& compilation)
    {
        auto& context = getWebGPUContext(device);

        if (!context.isValid())
            return;

        if (source.backend != ShaderBackend::WebGPU)
        {
            compilation.add(CompilationMessageType::Error,
                            "WebGPU takes WGSL; this source is another dialect");
            return;
        }

        auto wgsl = wgpu::ShaderSourceWGSL {};
        wgsl.code = wgpu::StringView {source.source.data(), source.source.size()};

        auto descriptor = wgpu::ShaderModuleDescriptor {};
        descriptor.nextInChain = &wgsl;

        program.module = context.getDevice().CreateShaderModule(&descriptor);
        program.bindings = scanWgslBindings(source.source);
        program.source = source.source;

        compilation.append(collectMessages(program.module));

        for (const auto& entry: entriesOf(source))
            if (!declaresEntry(source.source, entry))
                compilation.add(CompilationMessageType::Error,
                                "the WGSL declares no entry point " + entry);

        if (compilation.hasErrors())
        {
            LOG("WebGPU: a shader failed to compile\n", compilation.describe());
            program.module = nullptr;
        }
    }

    WebGPUShaderProgram program;
};

ShaderLibrary::ShaderLibrary(Device& device, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , groupShape(source.threadGroup)
    , pipelineConstants(source.hasPipelineConstants)
    , impl(device, source, compilation)
{
}

void* ShaderLibrary::nativeLibrary(const PipelineConstants&) const
{
    return nativeLibrary();
}

CompilationInfo ShaderLibrary::compilationInfo(const PipelineConstants&) const
{
    return compilation;
}

bool ShaderLibrary::isValid() const
{
    return impl->program.module != nullptr;
}

bool ShaderLibrary::isPrecompiled() const
{
    return false;
}

void* ShaderLibrary::nativeLibrary() const
{
    return const_cast<WebGPUShaderProgram*>(&impl->program);
}
} // namespace eacp::GPU
