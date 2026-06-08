add_executable(Compute Main.cpp)
target_link_libraries(Compute PRIVATE eacp-gpu)

find_package(ResEmbed REQUIRED)
res_embed_add(Compute
        FILES Compute.metal Compute.hlsl
        NAMESPACE ComputeShaders
        CATEGORY ComputeShaders)

set_default_target_setting(Compute)
