add_executable(DownloadAndPlay
        Catalogue.cpp
        Downloader.cpp
        Main.cpp)

target_link_libraries(DownloadAndPlay PRIVATE eacp-video-view eacp-text eacp-network)

# The clip list is data, not code: embedded at build time so the app carries it
# with no file to ship or locate, and read back through a Miro-reflected type
# (Catalogue.h) rather than hand-written parsing. The ResEmbed runtime key is
# the file's basename, so Clips.json must keep that name.
find_package(ResEmbed REQUIRED)
res_embed_add(DownloadAndPlay
        FILES     "${CMAKE_CURRENT_SOURCE_DIR}/Clips.json"
        NAMESPACE Clips
        CATEGORY  Clips)

set(BUNDLE_ID "com.eacp.downloadandplay")

set_target_properties(DownloadAndPlay PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Download and Play"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(DownloadAndPlay)
set_default_target_setting(DownloadAndPlay)
