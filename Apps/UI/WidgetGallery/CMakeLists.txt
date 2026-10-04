eacp_add_app(WidgetGallery Main.cpp)
target_link_libraries(WidgetGallery PRIVATE eacp-ui)
set_target_properties(WidgetGallery PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(WidgetGallery)
set_default_target_setting(WidgetGallery)
