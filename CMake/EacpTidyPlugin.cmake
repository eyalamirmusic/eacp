# Builds the ExtraClangRules clang-tidy plugin as part of the normal build so
# ExtraClangRules/bin/eacp-clang-tidy — and every IDE pointed at it — works
# straight after `cmake --build build`. The plugin must compile against the
# pinned LLVM (no ABI stability for clang-tidy plugins), so it runs as an
# ExternalProject with its own CMAKE_PREFIX_PATH rather than joining this
# build. Its binary dir stays in the source tree because the wrapper script
# resolves the library there, independent of the main build dir's name.

set(EACP_LLVM_PREFIX "" CACHE PATH
        "Pinned LLVM prefix used to build the ExtraClangRules plugin")

function(eacp_setup_tidy_plugin)
    set(prefix "${EACP_LLVM_PREFIX}")

    if (NOT prefix AND DEFINED ENV{EACP_LLVM_PREFIX})
        set(prefix "$ENV{EACP_LLVM_PREFIX}")
    endif ()

    if (NOT prefix)
        foreach (candidate
                /opt/homebrew/opt/llvm@21
                /usr/local/opt/llvm@21
                /home/linuxbrew/.linuxbrew/opt/llvm@21)
            if (EXISTS "${candidate}/bin/clang-tidy")
                set(prefix "${candidate}")
                break()
            endif ()
        endforeach ()
    endif ()

    if (NOT EXISTS "${prefix}/bin/clang-tidy")
        message(STATUS
                "ExtraClangRules: pinned LLVM not found — skipping the eacp "
                "clang-tidy plugin. `brew install llvm@21` (or set "
                "EACP_LLVM_PREFIX) and reconfigure to enable the IDE checks.")
        return()
    endif ()

    set(pluginDir "${PROJECT_SOURCE_DIR}/ExtraClangRules/plugin")

    include(ExternalProject)
    ExternalProject_Add(EacpTidyPlugin
            SOURCE_DIR "${pluginDir}"
            BINARY_DIR "${pluginDir}/build"
            PREFIX "${CMAKE_BINARY_DIR}/EacpTidyPlugin"
            CMAKE_ARGS
                -DCMAKE_BUILD_TYPE=Release
                "-DCMAKE_PREFIX_PATH=${prefix}"
            BUILD_ALWAYS TRUE
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS "${pluginDir}/build/libEacpTidyChecks.so")

    message(STATUS
            "ExtraClangRules: eacp clang-tidy plugin builds against ${prefix}")
endfunction()
