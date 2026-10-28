# One binary that re-runs itself: the ctest cases launch it in overflow,
# memory and bench modes, so a crash or a measurement never shares the runner's
# process. EACP_RUN_BENCH=1 turns the timing case on.
nano_add_executable(AlternateSignalStackBench
        NO_MAIN
        SOURCES AlternateSignalStackBench.cpp
        TARGETS eacp-core)

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    eacp_force_optimization(AlternateSignalStackBench)
endif ()
