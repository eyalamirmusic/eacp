# The decode tests play the same four clips VideoViewDemo renders, out of
# the shared fetch cache — no copy step and no dependence on the test's
# working directory.
include(EacpFetchDemoClips)
eacp_fetch_demo_clips(video_test_clips)

nano_add_executable(VideoTests
        NO_MAIN
        SOURCES
            PlayerDecodeTests.cpp
            PlayerPerfTests.cpp
            TestMain.cpp
        TARGETS eacp-video)

target_compile_definitions(VideoTests PRIVATE
        EACP_VIDEO_TEST_MEDIA_DIR="${EACP_DEMO_CLIPS_DIR}")
