add_library(eacp-core STATIC)
target_include_directories(eacp-core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../)
target_link_libraries(eacp-core PRIVATE
        "-framework foundation"
        "-framework cocoa"
        "-framework corevideo"
        "-framework quartzcore")

set_default_warnings_level(eacp-core)

target_sources(eacp-core PRIVATE Core.cpp)

if (APPLE)
    target_sources(eacp-core PRIVATE Core.mm)
endif ()