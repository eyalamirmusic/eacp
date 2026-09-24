find_package(NanoTest REQUIRED)

add_subdirectory(TextEncoder)
add_subdirectory(Codec)
add_subdirectory(DiT)
add_subdirectory(Sampler)

# Every kernel the app builds, emitted in all three dialects and compared with
# the text under ShaderGolden/, as Tests/GPU's ShaderGoldenTests does for the
# library. Device-free, so it runs on every lane that builds the app.
nano_add_executable(SA3ShaderGoldenTests
        SOURCES
            ShaderGoldenTests.cpp
        TARGETS eacp-sa3-codec eacp-sa3-dit eacp-sa3-text-encoder)

target_include_directories(SA3ShaderGoldenTests PRIVATE
        ${PROJECT_SOURCE_DIR}/Tests/GPU)

target_compile_definitions(SA3ShaderGoldenTests PRIVATE
        SA3_SHADER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ShaderGolden")

if (TARGET eacp-spirv)
    target_link_libraries(SA3ShaderGoldenTests PRIVATE eacp-spirv)
    target_compile_definitions(SA3ShaderGoldenTests PRIVATE EACP_HAS_SPIRV=1)
endif ()
