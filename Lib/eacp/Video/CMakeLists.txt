add_library(eacp-video STATIC)

target_link_libraries(eacp-video PUBLIC eacp-core eacp-graphics)

# Portable half: recorder tier dispatch and the DisplayLink-driven off-screen
# tiers, plus the whole playback stack above the Decoder interface — frame
# queueing, playhead-to-frame selection and the playback clock are platform
# agnostic by design, so only Decoder-* is per-OS.
target_sources(eacp-video PRIVATE
        VideoRecorder.cpp
        SyntheticClip.cpp
        Decode/FrameImage.cpp
        Decode/FrameStream.cpp
        Decode/Player.cpp
        Demux/Mp4Demuxer.cpp)

if (APPLE)
    target_sources(eacp-video PRIVATE
            Decode/Decoder-Apple.mm
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
            Decode/Decoder-Windows.cpp
            Encoder-Windows.cpp
            ScreenCapture-Windows.cpp)

    # Media Foundation sink writer (H.264) and source reader, plus their GUIDs
    # and the PROPVARIANT helpers the reader's seek and duration queries use.
    # d3d11 is the decoder's: the source reader is bound to a D3D11 video device
    # so the decode runs on the GPU.
    target_link_libraries(eacp-video PRIVATE
            d3d11
            mfplat
            mfreadwrite
            mfuuid
            ole32
            propsys)
endif ()

set_default_target_setting(eacp-video)
eacp_enable_unity_build(eacp-video)
add_ide_sources(eacp-video)
