# It reads its results back in main(), which waits through the nested pump.
eacp_add_app(Compute Main.cpp Kernels.h NESTED_PUMP CONSOLE
        DESCRIPTION "Compute kernels dispatched and their results read back and checked.")
target_link_libraries(Compute PRIVATE eacp-gpu)
set_target_properties(Compute PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(Compute)

eacp_add_precompiled_shaders(Compute SOURCES Shaders.cpp)
