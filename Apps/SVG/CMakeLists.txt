add_executable(SVG Main.cpp)
target_link_libraries(SVG PRIVATE eacp-svg)

set(BUNDLE_ID "com.eacp.svg")

set(SVG_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/example.svg)
set_source_files_properties(${SVG_RESOURCES} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
target_sources(SVG PRIVATE ${SVG_RESOURCES})

set_target_properties(SVG PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "SVG App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID}
        RESOURCE "${SVG_RESOURCES}")

set_default_target_setting(SVG)
