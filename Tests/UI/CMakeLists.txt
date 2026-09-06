nano_add_executable(UITests
        SOURCES
            ClipMaskTests.cpp
            ClippedFillTests.cpp
            ComboBoxTests.cpp
            CoverageAtlasTests.cpp
            DrawListTests.cpp
            DragAndDropTests.cpp
            GradientRampsTests.cpp
            ImageBatchTests.cpp
            KeyboardTests.cpp
            ListBoxTests.cpp
            MaskCacheTests.cpp
            LayerTests.cpp
            ModuleShaderTests.cpp
            RepaintTests.cpp
            ShadowTests.cpp
            SliderTests.cpp
            TabsTests.cpp
            TextEditorTests.cpp
        TARGETS eacp-ui eacp-sprites)

# ModuleShaderTests emits every shader this tier builds as GLSL and compiles
# it with glslang wherever eacp-spirv is built, so a dialect regression in one
# of them fails at compile time rather than as a wrong pixel on the one lane
# with a Vulkan device. The helper is defined in Tests/GPU, which
# Tests/CMakeLists.txt adds before this directory.
eacp_enable_glsl_compile_checks(UITests)
