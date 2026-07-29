add_library(eacp-mesh STATIC)

find_package(Cgltf REQUIRED)

# Like Sprites, this builds on the GPU module's portable public API only, so
# every source is platform-agnostic C++ with no APPLE/WIN32 branches. It links
# eacp-graphics for Graphics::Image, which is what decodes a glTF's PNG and JPEG
# textures through the platform codec - the reason no image dependency is needed
# beside cgltf.
target_link_libraries(eacp-mesh
        PUBLIC eacp-gpu eacp-graphics
        PRIVATE cgltf::cgltf)

target_sources(eacp-mesh PRIVATE
        GltfLoader.cpp
        MeshData.cpp
        MeshRenderer.cpp
        MeshShader.cpp
        MeshTypes.cpp)

set_default_target_setting(eacp-mesh)
eacp_enable_unity_build(eacp-mesh)
add_ide_sources(eacp-mesh)
