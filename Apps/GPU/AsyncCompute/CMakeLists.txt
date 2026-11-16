eacp_add_app(AsyncCompute Main.cpp)
target_link_libraries(AsyncCompute PRIVATE eacp-gpu)
set_target_properties(AsyncCompute PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(AsyncCompute)

if (EMSCRIPTEN)
    eacp_web_executable(AsyncCompute WEBGPU NESTED_PUMP)
endif ()
