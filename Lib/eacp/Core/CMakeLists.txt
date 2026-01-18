add_library(eacp-core STATIC)
target_include_directories(eacp-core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../../)

if(EACP_IOS)
    target_link_libraries(eacp-core PRIVATE
            "-framework Foundation"
            "-framework UIKit"
            "-framework QuartzCore")
    target_compile_definitions(eacp-core PUBLIC EACP_IOS=1)
else()
    target_link_libraries(eacp-core PRIVATE
            "-framework Foundation"
            "-framework Cocoa"
            "-framework CoreVideo"
            "-framework QuartzCore")
endif()

set_default_target_setting(eacp-core)

target_sources(eacp-core PRIVATE Core.cpp)

if (APPLE)
    # Shared Apple code
    target_sources(eacp-core PRIVATE Core.mm)

    # Platform-specific code
    if(EACP_IOS)
        target_sources(eacp-core PRIVATE Core-iOS.mm)
    else()
        target_sources(eacp-core PRIVATE Core-macOS.mm)
    endif()
endif ()