add_executable(X11Host Main.cpp)
target_link_libraries(X11Host PRIVATE eacp-graphics)

# The host resolves its own eacp symbols and the plugin resolves its own: the
# executable exports nothing (no ENABLE_EXPORTS) and DynamicLibrary opens the
# module RTLD_LOCAL.
add_dependencies(X11Host X11Plugin)
target_compile_definitions(X11Host PRIVATE
        X11_PLUGIN_PATH="$<TARGET_FILE:X11Plugin>")

set_default_target_setting(X11Host)
