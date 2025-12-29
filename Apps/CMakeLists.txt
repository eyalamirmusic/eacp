add_executable(Console Main.cpp)
target_link_libraries(Console PRIVATE eacp)

set_target_properties(Console PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "App")