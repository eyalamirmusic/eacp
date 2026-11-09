#import <Metal/Metal.h>

#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "../Device/MetalErrors-Apple.h"
#include "PrecompiledShaders.h"
#include "ShaderSource.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Utils/Strings.h>

namespace eacp::GPU
{
namespace
{
constexpr std::string_view metalLibrarySuffix()
{
#if TARGET_OS_SIMULATOR
    return Precompiled::iosSimulatorMetalSuffix;
#elif TARGET_OS_IPHONE
    return Precompiled::iosMetalSuffix;
#else
    return Precompiled::macMetalSuffix;
#endif
}

std::string loadFailureOf(NSError* error)
{
    auto reason = std::string {"newLibraryWithData failed"};

    if (error != nil)
        reason += Strings::concat(": ", error.localizedDescription.UTF8String);

    return reason;
}

std::string metalCompilerLogOf(NSError* error)
{
    if (error == nil)
        return {};

    return error.localizedDescription.UTF8String;
}
} // namespace

struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source, CompilationInfo& messages)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();

        if (metalDevice == nil)
        {
            messages.add(CompilationMessageType::Error,
                         "there is no Metal device to compile the library on");
            return;
        }

        // An empty source is a build something declined to make, and whatever
        // declined it has already said why - see ComputeProgram::prepare. There
        // is nothing here to compile and nothing to log.
        if (source.source.empty())
        {
            messages.add(CompilationMessageType::Error,
                         "the source is empty: what built it declined to");
            return;
        }

        auto name =
            Precompiled::resourceName(Precompiled::keyFor(source), metalLibrarySuffix());

        if (!loadPrecompiledOrNeedSource(metalDevice, name, messages))
            return;

        auto code = @(source.source.c_str());
        auto compilerLog = std::string {};
        auto compile = [metalDevice, code, &compilerLog](NSError** error)
        {
            id<MTLLibrary> built = [metalDevice newLibraryWithSource:code
                                                             options:nil
                                                               error:error];
            compilerLog = metalCompilerLogOf(*error);
            return built;
        };

        library = Detail::buildRetryingTransientFailures<id<MTLLibrary>>(
            "compiling a Metal shader library", compile);

        const auto failed = library.get() == nil;
        messages.append(parseMetalLog({compilerLog, {}, failed}));

        if (failed && !messages.hasErrors())
            messages.add(CompilationMessageType::Error,
                         "the Metal compiler built no library and said nothing");
    }

    // Loads the precompiled library when there is one, and says whether the
    // source still has to be compiled: not once the binary has loaded, and not
    // under the require flag, which leaves the library invalid instead.
    bool loadPrecompiledOrNeedSource(id<MTLDevice> metalDevice,
                                     const std::string& name,
                                     CompilationInfo& messages)
    {
        auto blob = Precompiled::find(name);

        if (!blob)
            return Precompiled::fallBackAfterMiss(name, messages);

        // No copy: embedded bytes are static, and injected ones live as long.
        auto data = dispatch_data_create(blob.data(),
                                         blob.size(),
                                         dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
                                         ^{});

        NSError* error = nil;
        library = [metalDevice newLibraryWithData:data error:&error];
        dispatch_release(data);

        if (library)
        {
            precompiled = true;
            return false;
        }

        return Precompiled::fallBackAfterLoadFailure(
            name, loadFailureOf(error), messages);
    }

    ObjC::Ptr<NSObject<MTLLibrary>> library;
    bool precompiled = false;
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

bool ShaderLibrary::isValid() const
{
    return impl->library.get() != nil;
}

void* ShaderLibrary::nativeLibrary(const PipelineConstants&) const
{
    return nativeLibrary();
}

CompilationInfo ShaderLibrary::compilationInfo(const PipelineConstants&) const
{
    return compilation;
}

bool ShaderLibrary::isPrecompiled() const
{
    return impl->precompiled;
}

void* ShaderLibrary::nativeLibrary() const
{
    return (__bridge void*) impl->library.get();
}
} // namespace eacp::GPU
