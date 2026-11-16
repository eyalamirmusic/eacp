add_executable(ContentProcessRecovery Main.mm)
target_link_libraries(ContentProcessRecovery PRIVATE eacp-webview "-framework WebKit")
set_target_properties(ContentProcessRecovery PROPERTIES
        MACOSX_BUNDLE TRUE
        OBJCXX_STANDARD 20)

eacp_set_gui_subsystem(ContentProcessRecovery)
set_default_target_setting(ContentProcessRecovery)
