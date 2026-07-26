add_library(eacp-video-view STATIC)

# VideoView bridges the playback engine to the GPU display stack: it pulls a
# player's current frame and draws it with the sprite renderer. All portable
# C++ — the zero-copy wrap and decoding live behind eacp-video / eacp-gpu.
target_link_libraries(eacp-video-view PUBLIC eacp-video eacp-sprites)

target_sources(eacp-video-view PRIVATE
        VideoView.cpp)

set_default_target_setting(eacp-video-view)
eacp_enable_unity_build(eacp-video-view)
add_ide_sources(eacp-video-view)
