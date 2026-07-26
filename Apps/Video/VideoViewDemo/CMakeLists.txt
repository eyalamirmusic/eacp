add_executable(VideoViewDemo Main.cpp)
target_link_libraries(VideoViewDemo PRIVATE eacp-video-view)

# The demo clips are fetched at configure time into media/ (hash-pinned,
# git-ignored, shared with Tests/Video) — the same clips the WebView demo
# ServingHeavyContent serves, so the native and web demos play identical
# content. All four are bundled so the Clip menu can switch between them.
include(EacpFetchHeavyClips)
eacp_fetch_heavy_clips(HEAVY_CLIP_FILES)

# macOS: the clips ride in the bundle, where getBundleResourcePath finds them.
set_source_files_properties(${HEAVY_CLIP_FILES} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
        HEADER_FILE_ONLY TRUE)
target_sources(VideoViewDemo PRIVATE ${HEAVY_CLIP_FILES})

set(BUNDLE_ID "com.eacp.videoviewdemo")

set_target_properties(VideoViewDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "eacp Video"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID}
        RESOURCE "${HEAVY_CLIP_FILES}")

# Windows has no bundle: the app falls back to media/ beside the executable.
if (WIN32)
    add_custom_command(TARGET VideoViewDemo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:VideoViewDemo>/media"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HEAVY_CLIP_FILES}
            "$<TARGET_FILE_DIR:VideoViewDemo>/media")
endif ()

eacp_set_gui_subsystem(VideoViewDemo)
set_default_target_setting(VideoViewDemo)
