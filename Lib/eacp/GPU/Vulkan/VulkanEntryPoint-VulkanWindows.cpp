#include "VulkanLoader.h"

#include <eacp/Core/Utils/WinInclude.h>

// The Windows half of the runtime loader. vulkan-1.dll ships with the OS, so it
// loads on any machine; whether there is a driver behind it is a separate
// question, and one vkCreateInstance answers with VK_ERROR_INCOMPATIBLE_DRIVER.

namespace eacp::GPU::detail
{
PFN_vkGetInstanceProcAddr vulkanEntryPoint()
{
    // Never freed: the context this feeds is a process-wide singleton that
    // outlives every caller, and unloading the loader would strand every
    // function pointer resolved through it.
    static auto* library = LoadLibraryW(L"vulkan-1.dll");

    if (library == nullptr)
        return nullptr;

    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(library, "vkGetInstanceProcAddr"));
}
} // namespace eacp::GPU::detail
