add_executable(SVG Main.cpp)
target_link_libraries(SVG PRIVATE eacp-svg)

set(BUNDLE_ID "com.eacp.svg")

set(SVG_TEST_FILES ${CMAKE_SOURCE_DIR}/Tests/SVG/TestFiles)
set(SVG_RESOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/example.svg
        ${SVG_TEST_FILES}/bootstrap-camera.svg
        ${SVG_TEST_FILES}/bootstrap-heart-fill.svg
        ${SVG_TEST_FILES}/feather-activity.svg
        ${SVG_TEST_FILES}/feather-check-circle.svg
        ${SVG_TEST_FILES}/flag-fr.svg
        ${SVG_TEST_FILES}/flag-jp.svg
        ${SVG_TEST_FILES}/material-home.svg
        ${SVG_TEST_FILES}/simpleicons-github.svg
        ${SVG_TEST_FILES}/tabler-star.svg
        ${SVG_TEST_FILES}/twemoji-smile.svg)
set_source_files_properties(${SVG_RESOURCES} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
target_sources(SVG PRIVATE ${SVG_RESOURCES})

set_target_properties(SVG PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "SVG App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID}
        RESOURCE "${SVG_RESOURCES}")

eacp_set_gui_subsystem(SVG)
set_default_target_setting(SVG)
