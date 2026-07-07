# Build-time host tool: converts one source PNG/JPEG into a platform icon
# container. Invoked from eacp_set_app_icon(... IMAGE ...) via a custom command.
add_executable(eacp-icon-tool
        Main.cpp
        IconContainers.cpp)

target_link_libraries(eacp-icon-tool PRIVATE eacp-graphics)

set_default_target_setting(eacp-icon-tool)
add_ide_sources(eacp-icon-tool)
