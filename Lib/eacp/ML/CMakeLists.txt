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

if (NOT EACP_HAS_GPU)
    return ()
endif ()

# The tensors, safetensors loader and compute kernels the Stable Audio models
# run on, over eacp-gpu wherever it builds. Its Tensor and DType share the
# eacp::ML namespace with eacp-ml-graph's, so no target links both.
add_library(eacp-ml-kernels STATIC)

target_link_libraries(eacp-ml-kernels PUBLIC eacp-gpu)

target_sources(eacp-ml-kernels PRIVATE
        Tensor/Tensor.cpp
        Loader/Json.cpp
        Loader/SafetensorsFile.cpp
        Kernels/Linear.cpp
        Kernels/Norm.cpp
        Kernels/Activation.cpp
        Kernels/RoPE.cpp
        Kernels/Attention.cpp
        Kernels/BandedAttention.cpp
        Kernels/SwiGLU.cpp
        Kernels/TensorOps.cpp)

set_default_target_setting(eacp-ml-kernels)
eacp_enable_unity_build(eacp-ml-kernels)
add_ide_sources(eacp-ml-kernels)

if (NOT EACP_HAS_COREML)
    return ()
endif ()

add_library(eacp-ml STATIC)

target_link_libraries(eacp-ml PUBLIC eacp-ml-graph eacp-gpu)

target_compile_definitions(eacp-ml PUBLIC EACP_HAS_COREML=1)

target_sources(eacp-ml PRIVATE
        Model/ComputePlan.cpp
        Model/MultiArray.mm
        Model/Options.cpp
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
