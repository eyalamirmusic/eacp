add_subdirectory(TextEncoder)
add_subdirectory(Codec)
add_subdirectory(DiT)
add_subdirectory(Sampler)
add_subdirectory(Tests)

add_library(eacp-sa3-checkpoints STATIC Checkpoints.cpp)
target_include_directories(eacp-sa3-checkpoints PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(eacp-sa3-checkpoints PUBLIC eacp-core PRIVATE eacp-network)
set_default_target_setting(eacp-sa3-checkpoints)

add_executable(StableAudio3 Main.cpp)

target_link_libraries(StableAudio3 PRIVATE
        eacp-sa3-checkpoints
        eacp-sa3-text-encoder
        eacp-sa3-dit
        eacp-sa3-codec
        eacp-sa3-sampler)

set_default_target_setting(StableAudio3)
