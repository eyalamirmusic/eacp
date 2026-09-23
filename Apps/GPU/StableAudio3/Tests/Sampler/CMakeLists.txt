nano_add_executable(SA3SamplerFastTests
        SOURCES
            FastMathTests.cpp
            AnalyticFormulaTests.cpp
        TARGETS eacp-sa3-sampler)

set(sa3_sampler_medium_checkpoint_path
        "$ENV{HOME}/.cache/huggingface/hub/models--stabilityai--stable-audio-3-medium/snapshots/27b5a21b791b1b033d193a9e1e3ce78493f102f9/model.safetensors")

nano_add_executable(SA3SamplerMediumVariesWithSeedTests
        SOURCES
            MediumVariesWithSeedTests.cpp
        TARGETS eacp-sa3-sampler)

target_compile_definitions(SA3SamplerMediumVariesWithSeedTests PRIVATE
        SA3_DIT_MEDIUM_CHECKPOINT_PATH="${sa3_sampler_medium_checkpoint_path}")
