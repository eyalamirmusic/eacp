eacp_add_app(HelloGPU Main.cpp)
target_link_libraries(HelloGPU PRIVATE eacp-gpu eacp-text)
set_default_target_setting(HelloGPU)
