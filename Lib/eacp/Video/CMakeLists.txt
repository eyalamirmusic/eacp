add_library(eacp-video STATIC)

target_link_libraries(eacp-video PUBLIC eacp-core eacp-graphics)

if (APPLE)
    target_sources(eacp-video PRIVATE
            VideoRecorder-Apple.mm)

    target_link_libraries(eacp-video PRIVATE
            "-framework AVFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework Foundation")
endif ()

set_default_target_setting(eacp-video)
eacp_enable_unity_build(eacp-video)
add_ide_sources(eacp-video)
