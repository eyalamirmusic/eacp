# An extern "C" export throws on purpose; /EHc- after CMake's /EHsc is a D9025.
if (MSVC)
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif ()

eacp_add_plugin(CrashingLibrary CrashingLibrary.cpp)
target_link_libraries(CrashingLibrary PRIVATE eacp-core)
eacp_add_crash_symbols(CrashingLibrary "")
