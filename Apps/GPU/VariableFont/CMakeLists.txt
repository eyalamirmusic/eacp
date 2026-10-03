eacp_add_app(VariableFont Main.cpp DISPLAY_NAME "Variable Font")
target_link_libraries(VariableFont PRIVATE eacp-sprites eacp-text)

# The face is data, not code: embedded so the app carries it with nothing to
# install and no file to find at runtime. The ResEmbed runtime key is the
# file's basename, so InterVariable-Subset.ttf must keep that name.
find_package(ResEmbed REQUIRED)
res_embed_add(VariableFont
        FILES     "${CMAKE_CURRENT_SOURCE_DIR}/InterVariable-Subset.ttf"
        NAMESPACE VariableFontAssets
        CATEGORY  VariableFontAssets)

set_target_properties(VariableFont PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(VariableFont)
set_default_target_setting(VariableFont)
