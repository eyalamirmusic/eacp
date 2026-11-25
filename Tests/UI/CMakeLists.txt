nano_add_executable(UITests
        SOURCES
            ClipMaskTests.cpp
            ClippedFillTests.cpp
            ComboBoxTests.cpp
            ComponentRemovalTests.cpp
            CoverageAtlasTests.cpp
            CursorTests.cpp
            DrawListTests.cpp
            DragAndDropTests.cpp
            FileDragTests.cpp
            FileDropTests.cpp
            GPUComponentTests.cpp
            GradientRampsTests.cpp
            GradientTests.cpp
            ImageBatchTests.cpp
            KeyboardTests.cpp
            ListBoxTests.cpp
            MagnifyTests.cpp
            MaskCacheTests.cpp
            MenuBarComponentTests.cpp
            LayerTests.cpp
            ModuleShaderTests.cpp
            MouseEventTests.cpp
            OnlineResourceMonitorTests.cpp
            OverlayViewTests.cpp
            PopupMenuTests.cpp
            RepaintTests.cpp
            SafePointerTests.cpp
            ShadowTests.cpp
            SliderTests.cpp
            TabsTests.cpp
            TextEditorTests.cpp
            ThemeTests.cpp
            TouchScrollTests.cpp
            ViewportTests.cpp
        TARGETS eacp-ui eacp-sprites eacp-ui-network)

# A window still open when its process exits, closed from an exit handler or an
# earlier static's destructor, which ExitTests runs as a child: exit can only be
# seen from outside, and the web launches no process.
if (NOT EMSCRIPTEN)
    target_sources(UITests PRIVATE ExitTests.cpp)

    add_executable(ExitHarness ExitHarness.cpp)
    target_link_libraries(ExitHarness PRIVATE eacp-ui)

    target_compile_definitions(UITests PRIVATE
            EACP_EXIT_HARNESS="$<TARGET_FILE:ExitHarness>")
    add_dependencies(UITests ExitHarness)
endif ()

# ModuleShaderTests emits every shader this tier builds as GLSL and compiles it
# with glslang wherever eacp-spirv is built. The helper comes from Tests/GPU.
eacp_enable_glsl_compile_checks(UITests)
