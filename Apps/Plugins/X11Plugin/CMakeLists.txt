eacp_add_plugin(X11Plugin Plugin.cpp)
target_link_libraries(X11Plugin PRIVATE eacp-gpu)

# A module links with undefined symbols allowed, so a missing platform source
# would surface as a failed dlopen in the host rather than as a link error
# here. This is only a plugin's own build, not a rule for every one of them.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(X11Plugin PRIVATE LINKER:--no-undefined)
endif ()
