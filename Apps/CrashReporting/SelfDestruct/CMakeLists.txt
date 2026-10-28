eacp_add_app(SelfDestruct Main.cpp ReportWindow.cpp)
target_link_libraries(SelfDestruct PRIVATE eacp-crash-faults eacp-ui)

if (APPLE)
    target_sources(SelfDestruct PRIVATE PersistentState-macOS.mm)
else ()
    target_sources(SelfDestruct PRIVATE PersistentState-Default.cpp)
endif ()
set_target_properties(SelfDestruct PROPERTIES MACOSX_BUNDLE TRUE)

eacp_enable_crash_reporting(SelfDestruct)
eacp_set_gui_subsystem(SelfDestruct)
set_default_target_setting(SelfDestruct)
