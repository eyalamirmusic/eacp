add_subdirectory(TextEncoder)
add_subdirectory(Codec)
add_subdirectory(DiT)
add_subdirectory(Sampler)
add_subdirectory(Tests)

add_executable(StableAudio3 Main.cpp)

target_link_libraries(StableAudio3 PRIVATE
        eacp-sa3-text-encoder
        eacp-sa3-dit
        eacp-sa3-codec
        eacp-sa3-sampler)

set_default_target_setting(StableAudio3)
