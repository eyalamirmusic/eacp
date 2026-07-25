include(CPM)

# Headers only, deliberately: eacp links no Vulkan import library and resolves
# every entry point through the loader at runtime (see Vulkan/VulkanLoader.h).
# That removes the LunarG SDK from the build requirements on every platform --
# nothing here needs its shader compiler, because the SPIR-V a shader graph turns
# into is emitted by Codegen/SpirvEmitter.cpp -- and it lets an app start on a
# machine with no Vulkan at all, where a link-time dependency on vulkan-1.lib
# would instead stop the process from loading.
CPMAddPackage(
        NAME VulkanHeaders
        GITHUB_REPOSITORY KhronosGroup/Vulkan-Headers
        GIT_TAG v1.4.357
        OPTIONS
        "VULKAN_HEADERS_ENABLE_MODULE OFF"
        "VULKAN_HEADERS_ENABLE_INSTALL OFF")
