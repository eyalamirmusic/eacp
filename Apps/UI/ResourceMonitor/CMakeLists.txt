add_executable(ResourceMonitor Main.cpp)
target_link_libraries(ResourceMonitor PRIVATE eacp-ui-network)

# The same catalogue DownloadAndPlay offers, embedded from its directory so the
# two stay one list: what is fetched here plays there.
find_package(ResEmbed REQUIRED)
res_embed_add(ResourceMonitor
        FILES     "${CMAKE_SOURCE_DIR}/Apps/Video/DownloadAndPlay/Clips.json"
        NAMESPACE Clips
        CATEGORY  Clips)

set(BUNDLE_ID "com.eacp.resourcemonitor")

set_target_properties(ResourceMonitor PROPERTIES
        MACOSX_BUNDLE TRUE
        EACP_COMPANY_NAME "eacp"
        MACOSX_BUNDLE_BUNDLE_NAME "Resource Monitor"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(ResourceMonitor)
set_default_target_setting(ResourceMonitor)
