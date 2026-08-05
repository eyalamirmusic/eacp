nano_add_executable(InversionCodegenTests
        SOURCES InversionCodegenTests.cpp)

# MiroFormats: Miro's built-in zod/ts/backend/ts-server/bridge formats.
# eacp-webview-codegen: eacp's events/hooks formats.
# Both are OBJECT libraries; linking them splices the static-init
# format registrations directly into the test binary so codegenMain
# sees the full Format set the codegen executables do.
target_link_libraries(InversionCodegenTests PRIVATE MiroFormats)

if (TARGET eacp-webview-codegen)
    # eacp-core alongside it, exactly as eacp_add_webview_app links the
    # app's Schema_Codegen executable: the formatters call into
    # eacp::Strings, which the object lib only sees the headers of.
    target_link_libraries(InversionCodegenTests PRIVATE
            eacp-webview-codegen eacp-core)
endif ()
