nano_add_executable(SA3CodecFastTests
        SOURCES
            FastMathTests.cpp
        TARGETS eacp-sa3-codec)

function(sa3_codec_add_golden_test name source)
    nano_add_executable(${name}
            SOURCES
                ${source}
                GoldenFixture.cpp
            TARGETS eacp-sa3-codec eacp-sa3-checkpoints)

    target_compile_definitions(${name} PRIVATE
            SA3_CODEC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/GoldenData")
endfunction()

sa3_codec_add_golden_test(SA3CodecPatchedPretransformGoldenTests PatchedPretransformGoldenTests.cpp)
sa3_codec_add_golden_test(SA3CodecFoldBlockGoldenTests FoldBlockGoldenTests.cpp)
sa3_codec_add_golden_test(SA3CodecBottleneckGoldenTests BottleneckGoldenTests.cpp)
sa3_codec_add_golden_test(SA3CodecRoundTripGoldenTests RoundTripGoldenTests.cpp)

nano_add_executable(SA3CodecSameLGoldenTests
        SOURCES
            SameLGoldenTests.cpp
            GoldenFixture.cpp
        TARGETS eacp-sa3-codec eacp-sa3-checkpoints)

target_compile_definitions(SA3CodecSameLGoldenTests PRIVATE
        SA3_CODEC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/GoldenData")

nano_add_executable(SA3CodecSameLDecodeVariesWithLatentTests
        SOURCES
            SameLDecodeVariesWithLatentTests.cpp
        TARGETS eacp-sa3-codec eacp-sa3-checkpoints)

