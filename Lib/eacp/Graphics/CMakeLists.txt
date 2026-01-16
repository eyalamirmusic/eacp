add_library(eacp-graphics STATIC)

target_link_libraries(eacp-graphics PUBLIC eacp-core PRIVATE
        "-framework foundation"
        "-framework cocoa"
        "-framework corevideo"
        "-framework quartzcore"
        "-framework carbon")

set_default_target_setting(eacp-graphics)

target_sources(eacp-graphics PRIVATE Graphics.cpp)

if (APPLE)
    target_sources(eacp-graphics PRIVATE Graphics.mm)
endif ()