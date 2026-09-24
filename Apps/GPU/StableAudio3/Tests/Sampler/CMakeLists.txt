nano_add_executable(SA3SamplerFastTests
        SOURCES
            FastMathTests.cpp
            AnalyticFormulaTests.cpp
        TARGETS eacp-sa3-sampler)

nano_add_executable(SA3SamplerMediumVariesWithSeedTests
        SOURCES
            MediumVariesWithSeedTests.cpp
        TARGETS eacp-sa3-sampler eacp-sa3-checkpoints)
