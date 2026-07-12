add_executable(PluginHost Main.cpp)
target_link_libraries(PluginHost PRIVATE eacp-graphics)

add_dependencies(PluginHost DemoPlugin)
target_compile_definitions(PluginHost PRIVATE
        DEMO_PLUGIN_PATH="$<TARGET_FILE:DemoPlugin>")
