add_library(eacp-sa3-text-encoder STATIC)

target_link_libraries(eacp-sa3-text-encoder PUBLIC eacp-ml)

target_sources(eacp-sa3-text-encoder PRIVATE
        Tokenizer/BpeTokenizer.cpp
        Encoder/GemmaAttention.cpp
        Encoder/T5GemmaEncoder.cpp
        SA3TextEncoder.cpp)

set_default_target_setting(eacp-sa3-text-encoder)
eacp_enable_unity_build(eacp-sa3-text-encoder)
add_ide_sources(eacp-sa3-text-encoder)
