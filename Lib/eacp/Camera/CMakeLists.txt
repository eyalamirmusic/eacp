add_library(eacp-camera STATIC)

target_link_libraries(eacp-camera PUBLIC eacp-core eacp-graphics)

target_sources(eacp-camera PRIVATE
        Camera.cpp)

if (APPLE)
    target_sources(eacp-camera PRIVATE
            Camera-macOS.mm)
    target_link_libraries(eacp-camera PRIVATE
            "-framework AVFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework Foundation")
elseif (WIN32)
    target_sources(eacp-camera PRIVATE
            Camera-Windows.cpp)
endif ()

set_default_target_setting(eacp-camera)
eacp_enable_unity_build(eacp-camera)
add_ide_sources(eacp-camera)
