eacp_add_app(PathCoverage Main.cpp
        DESCRIPTION "Analytic path coverage computed in a kernel, under both fill rules.")
target_link_libraries(PathCoverage PRIVATE eacp-gpuwidgets)
set_target_properties(PathCoverage PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(PathCoverage)
