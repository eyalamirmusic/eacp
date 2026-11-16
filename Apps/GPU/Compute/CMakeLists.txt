eacp_add_app(Compute Main.cpp Kernels.h)
target_link_libraries(Compute PRIVATE eacp-gpu)
set_target_properties(Compute PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(Compute)

eacp_add_precompiled_shaders(Compute SOURCES Shaders.cpp)

# It reads its results back in main(), which waits through the nested pump.
if (EMSCRIPTEN)
    eacp_web_executable(Compute WEBGPU NESTED_PUMP)
endif ()
