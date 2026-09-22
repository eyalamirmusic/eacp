add_library(eacp-ml STATIC)

target_link_libraries(eacp-ml PUBLIC eacp-gpu)

target_sources(eacp-ml PRIVATE
        Tensor/Tensor.cpp
        Loader/Json.cpp
        Loader/SafetensorsFile.cpp
        Kernels/Linear.cpp
        Kernels/Norm.cpp
        Kernels/Activation.cpp
        Kernels/RoPE.cpp
        Kernels/Attention.cpp
        Kernels/SwiGLU.cpp)

set_default_target_setting(eacp-ml)
eacp_enable_unity_build(eacp-ml)
add_ide_sources(eacp-ml)
