add_library(eacp-video STATIC)

target_link_libraries(eacp-video PUBLIC eacp-core eacp-graphics)

# Portable half: tier dispatch and the DisplayLink-driven off-screen tiers.
target_sources(eacp-video PRIVATE
        VideoRecorder.cpp)

if (APPLE)
    target_sources(eacp-video PRIVATE
            Encoder-Apple.mm
            ScreenCapture-Apple.mm)

    target_link_libraries(eacp-video PRIVATE
            "-framework AVFoundation"
            "-framework AppKit"
            "-framework CoreGraphics"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework Foundation"
            "-framework ScreenCaptureKit")
elseif (WIN32)
    target_sources(eacp-video PRIVATE
            Encoder-Windows.cpp
            ScreenCapture-Windows.cpp)

    # Media Foundation sink writer (H.264) + its GUIDs.
    target_link_libraries(eacp-video PRIVATE
            mfplat
            mfreadwrite
            mfuuid
            ole32)
endif ()

set_default_target_setting(eacp-video)
eacp_enable_unity_build(eacp-video)
add_ide_sources(eacp-video)
