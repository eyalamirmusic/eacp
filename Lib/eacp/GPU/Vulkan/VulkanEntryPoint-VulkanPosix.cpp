#include "VulkanLoader.h"

#include <dlfcn.h>

// The POSIX half of the runtime loader, shared by macOS and Linux.

namespace eacp::GPU::detail
{
namespace
{
void* openVulkanLibrary()
{
    // The loader where there is one, then MoltenVK directly: a Homebrew
    // molten-vk install is the driver without a loader in front of it.
    for (const auto* name: {"libvulkan.1.dylib",
                            "libvulkan.so.1",
                            "libvulkan.dylib",
                            "libMoltenVK.dylib"})
        if (auto* library = dlopen(name, RTLD_NOW | RTLD_LOCAL))
            return library;

    // Nothing to open, which on iOS is the expected answer rather than a
    // failure: MoltenVK is linked into the executable there (there is no loader
    // to link against), so its symbols are already in the running image.
    return RTLD_DEFAULT;
}
} // namespace

PFN_vkGetInstanceProcAddr vulkanEntryPoint()
{
    // Never closed, for the reason given in the Windows half.
    static auto* library = openVulkanLibrary();

    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(library, "vkGetInstanceProcAddr"));
}
} // namespace eacp::GPU::detail
