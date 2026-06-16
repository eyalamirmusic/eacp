add_library(eacp-camera-view STATIC)

# CameraView bridges the capture engine to the GPU display stack: it pulls the
# camera's latest frame and draws it with the sprite renderer. All portable C++
# — the zero-copy wrap and frame delivery live behind eacp-camera / eacp-gpu.
target_link_libraries(eacp-camera-view PUBLIC eacp-camera eacp-sprites)

target_sources(eacp-camera-view PRIVATE
        CameraView.cpp)

set_default_target_setting(eacp-camera-view)
eacp_enable_unity_build(eacp-camera-view)
add_ide_sources(eacp-camera-view)
