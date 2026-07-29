set(mesh_test_sources
        Common.cpp
        GltfLoaderTests.cpp
        MeshTransformTests.cpp
        TestMain.cpp)

# The rendered cases ride the same off-screen path the GPU suite does
# (View::renderToImage over a GPUView): Metal off-screen textures on Apple,
# D3D12 off-screen render targets on Windows. Everything else in this suite is
# a property of MeshData and needs no device at all - which is the point of the
# loader producing a plain struct.
if (APPLE OR WIN32)
    list(APPEND mesh_test_sources MeshRenderTests.cpp)
endif ()

nano_add_executable(MeshTests
        NO_MAIN
        SOURCES
            ${mesh_test_sources}
        TARGETS eacp-mesh)
