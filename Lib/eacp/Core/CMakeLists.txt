add_library(eacp-core STATIC)
target_include_directories(eacp-core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../)

if (APPLE)
    target_sources(eacp-core PRIVATE Core.mm)
    target_link_libraries(eacp-core PRIVATE
            "-framework Foundation")
    if (EACP_IOS)
        target_sources(eacp-core PRIVATE Core-iOS.mm)
    else ()
        target_sources(eacp-core PRIVATE Core-macOS.mm)
        target_link_libraries(eacp-core PRIVATE
                "-framework Cocoa")
    endif ()

endif ()


set_default_target_setting(eacp-core)

target_sources(eacp-core PRIVATE Core.cpp)