add_library(eacp-mesh STATIC)

# Like Sprites, this builds on the GPU module's portable public API only, so
# every source is platform-agnostic C++ with no APPLE/WIN32 branches.
#
# There is no third-party glTF parser here on purpose: the JSON comes from
# Miro::Json, which eacp-core already links publicly, and everything above it -
# the GLB container, the accessor indirection, the buffer resolution - is
# GltfReader. eacp-graphics is for Graphics::Image, which decodes a model's PNG
# and JPEG textures through the platform codec.
target_link_libraries(eacp-mesh PUBLIC eacp-gpu eacp-graphics eacp-core)

target_sources(eacp-mesh PRIVATE
        GltfLoader.cpp
        GltfReader.cpp
        MeshData.cpp
        MeshRenderer.cpp
        MeshShader.cpp
        MeshTypes.cpp)

set_default_target_setting(eacp-mesh)
eacp_enable_unity_build(eacp-mesh)
add_ide_sources(eacp-mesh)
