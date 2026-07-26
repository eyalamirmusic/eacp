# The decode tests play the same four clips the PlayingHeavyContent demo
# renders, out of the shared fetch cache — no copy step and no dependence on
# the test's working directory.
include(EacpFetchHeavyClips)
eacp_fetch_heavy_clips(video_test_clips)

nano_add_executable(VideoTests
        NO_MAIN
        SOURCES
            PlayerDecodeTests.cpp
            PlayerPerfTests.cpp
            TestMain.cpp
        TARGETS eacp-video)

target_compile_definitions(VideoTests PRIVATE
        EACP_VIDEO_TEST_MEDIA_DIR="${EACP_HEAVY_CLIPS_DIR}")
