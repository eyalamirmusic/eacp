add_library(eacp-ml-graph STATIC)

target_link_libraries(eacp-ml-graph PUBLIC eacp-core eacp-gpu-codegen)

target_sources(eacp-ml-graph PRIVATE
        Graph/Elementwise.cpp
        Graph/Graph.cpp
        Graph/Lowering.cpp
        MIL/Blob.cpp
        MIL/Half.cpp
        MIL/MILWriter.cpp
        MIL/Package.cpp
        MIL/Protobuf.cpp)

set_default_target_setting(eacp-ml-graph)
eacp_enable_unity_build(eacp-ml-graph)
add_ide_sources(eacp-ml-graph)

if (NOT EACP_HAS_COREML)
    return ()
endif ()

add_library(eacp-ml STATIC)

target_link_libraries(eacp-ml PUBLIC eacp-ml-graph eacp-gpu)

target_compile_definitions(eacp-ml PUBLIC EACP_HAS_COREML=1)

target_sources(eacp-ml PRIVATE
        Model/MultiArray.mm
        Model/MultiArrayNative.h
        Model/Model.mm)

target_link_libraries(eacp-ml PRIVATE
        "-framework Accelerate"
        "-framework CoreML"
        "-framework CoreVideo"
        "-framework Foundation")

set_default_target_setting(eacp-ml)
eacp_enable_unity_build(eacp-ml)
add_ide_sources(eacp-ml)
