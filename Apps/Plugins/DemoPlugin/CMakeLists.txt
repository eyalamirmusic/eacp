eacp_add_plugin(DemoPlugin Plugin.cpp)
target_link_libraries(DemoPlugin PRIVATE eacp-gpu)

# A module links with undefined symbols allowed, so a missing platform source
# would surface as a failed dlopen in the host rather than as a link error
# here. This is only a plugin's own build, not a rule for every one of them.
if (LINUX)
    target_link_options(DemoPlugin PRIVATE LINKER:--no-undefined)
endif ()
