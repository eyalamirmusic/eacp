add_library(eacp-sa3-codec STATIC)

target_link_libraries(eacp-sa3-codec PUBLIC eacp-ml)

target_sources(eacp-sa3-codec PRIVATE
        CodecTransformerBlock.cpp
        GpuOps.cpp
        HostMatrix.cpp
        PatchedPretransform.cpp
        SA3Codec.cpp
        SoftNormBottleneck.cpp
        TransformerResamplingBlock.cpp
        WavFile.cpp
        WNConv1d.cpp)

set_default_target_setting(eacp-sa3-codec)
eacp_enable_unity_build(eacp-sa3-codec)
add_ide_sources(eacp-sa3-codec)
