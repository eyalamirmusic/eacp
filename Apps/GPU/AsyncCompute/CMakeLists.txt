eacp_add_app(AsyncCompute Main.cpp NESTED_PUMP CONSOLE
        DESCRIPTION "One kernel committed blocking and asynchronously, timed and compared.")
target_link_libraries(AsyncCompute PRIVATE eacp-gpu)
set_target_properties(AsyncCompute PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(AsyncCompute)
