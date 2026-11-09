nano_add_executable(UITests
        SOURCES
            ClipMaskTests.cpp
            ClippedFillTests.cpp
            ComboBoxTests.cpp
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

# ModuleShaderTests emits every shader this tier builds as GLSL and compiles it
# with glslang wherever eacp-spirv is built. The helper comes from Tests/GPU.
eacp_enable_glsl_compile_checks(UITests)
