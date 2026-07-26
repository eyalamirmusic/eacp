add_executable(PlayingHeavyContent Main.cpp)
target_link_libraries(PlayingHeavyContent PRIVATE eacp-video-view)

# The demo clips are fetched at configure time into media/ (hash-pinned,
# git-ignored, shared with Tests/Video) — the same four clips
# ServingHeavyContent serves through the webview, so the native and web demos
# play identical content. Four clips rather than one, because the interaction
# being demonstrated is switching between heavy clips that all keep decoding.
include(EacpFetchHeavyClips)
eacp_fetch_heavy_clips(HEAVY_CLIP_FILES)

# macOS: the clips ride in the bundle, where getBundleResourcePath finds them.
set_source_files_properties(${HEAVY_CLIP_FILES} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
        HEADER_FILE_ONLY TRUE)
target_sources(PlayingHeavyContent PRIVATE ${HEAVY_CLIP_FILES})

set(BUNDLE_ID "com.eacp.playingheavycontent")

set_target_properties(PlayingHeavyContent PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Playing Heavy Content"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID}
        RESOURCE "${HEAVY_CLIP_FILES}")

# Windows has no bundle: the app falls back to media/ beside the executable.
if (WIN32)
    add_custom_command(TARGET PlayingHeavyContent POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:PlayingHeavyContent>/media"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HEAVY_CLIP_FILES}
            "$<TARGET_FILE_DIR:PlayingHeavyContent>/media")
endif ()

eacp_set_gui_subsystem(PlayingHeavyContent)
set_default_target_setting(PlayingHeavyContent)
