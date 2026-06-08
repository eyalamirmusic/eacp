#import <Metal/Metal.h>

#include "ShaderLibrary.h"

#include "../Device/Device.h"
#include "ShaderSource.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct ShaderLibrary::Native
{
    Native(Device& device, const ShaderSource& source)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();

        if (metalDevice == nil)
            return;

        NSError* error = nil;
        auto code = @(source.source.c_str());
        library = [metalDevice newLibraryWithSource:code options:nil error:&error];

        if (library.get() == nil && error != nil)
            LOG(error.localizedDescription.UTF8String);
    }

    ObjC::Ptr<NSObject<MTLLibrary>> library;
};

ShaderLibrary::ShaderLibrary(Device& device, const ShaderSource& source)
    : vertexEntryName(source.vertexEntry)
    , fragmentEntryName(source.fragmentEntry)
    , computeEntryName(source.computeEntry)
    , impl(device, source)
{
}

bool ShaderLibrary::isValid() const
{
    return impl->library.get() != nil;
}

void* ShaderLibrary::nativeLibrary() const
{
    return (__bridge void*) impl->library.get();
}
} // namespace eacp::GPU
