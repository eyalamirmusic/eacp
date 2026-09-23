nano_add_executable(SA3CodecFastTests
        SOURCES
            FastMathTests.cpp
        TARGETS eacp-sa3-codec)

set(sa3_codec_checkpoint_path
        "$ENV{HOME}/.cache/huggingface/hub/models--stabilityai--stable-audio-3-small-music/snapshots/0fef1392cd842149a2b6d445e181c97608faac06/model.safetensors")

function(sa3_codec_add_golden_test name source)
    nano_add_executable(${name}
            SOURCES
                ${source}
                GoldenFixture.cpp
            TARGETS eacp-sa3-codec)

    target_compile_definitions(${name} PRIVATE
            SA3_CODEC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/GoldenData"
            SA3_CODEC_CHECKPOINT_PATH="${sa3_codec_checkpoint_path}")
endfunction()

sa3_codec_add_golden_test(SA3CodecPatchedPretransformGoldenTests PatchedPretransformGoldenTests.cpp)
sa3_codec_add_golden_test(SA3CodecFoldBlockGoldenTests FoldBlockGoldenTests.cpp)
sa3_codec_add_golden_test(SA3CodecBottleneckGoldenTests BottleneckGoldenTests.cpp)
sa3_codec_add_golden_test(SA3CodecRoundTripGoldenTests RoundTripGoldenTests.cpp)

set(sa3_samel_checkpoint_path
        "$ENV{HOME}/.cache/huggingface/hub/models--stabilityai--SAME-L/snapshots/41acf79dd242877d6499a1108ca5dba5d5eecfc5/model.safetensors")

nano_add_executable(SA3CodecSameLGoldenTests
        SOURCES
            SameLGoldenTests.cpp
            GoldenFixture.cpp
        TARGETS eacp-sa3-codec)

target_compile_definitions(SA3CodecSameLGoldenTests PRIVATE
        SA3_CODEC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/GoldenData"
        SA3_SAMEL_CHECKPOINT_PATH="${sa3_samel_checkpoint_path}")
