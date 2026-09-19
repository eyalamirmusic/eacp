# The OpenGL half of the Linux GPU seam, and FindVulkanBackend.cmake's twin.
#
# Nothing here links libEGL or libGL. The loader is the glad2 C sources carried
# in ThirdParty/glad; EGL's entry points come from a dlopen("libEGL.so.1") the
# backend does itself and GL's from eglGetProcAddress, so a machine with no EGL
# builds the same binary and reports Device::isValid() false - exactly the rule
# volkInitialize() gives the Vulkan backend.
#
# One C target of its own, so the generated sources never join an eacp unity
# build and their warnings are silenced in one place.

if (NOT TARGET eacp-gl)
    set(EACP_GLAD_DIR "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/glad")

    add_library(eacp-gl STATIC
            "${EACP_GLAD_DIR}/src/egl.c"
            "${EACP_GLAD_DIR}/src/gl.c")

    target_include_directories(eacp-gl SYSTEM PUBLIC
            "${EACP_GLAD_DIR}/include")

    # The loaders are opened by name at runtime; only the opener is linked.
    target_link_libraries(eacp-gl PUBLIC ${CMAKE_DL_LIBS})

    # Third-party sources, not clean under eacp's warning set.
    target_compile_options(eacp-gl PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-w>)

    set_target_properties(eacp-gl PROPERTIES
            C_STANDARD 99
            UNITY_BUILD OFF
            POSITION_INDEPENDENT_CODE ON
            FOLDER "${CMAKE_FOLDER}")
endif ()
