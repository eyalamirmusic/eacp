# The flags an Emscripten build of eacp shares, in one place so libraries, their
# fetched dependencies and apps agree. Included by the top-level CMakeLists.txt
# ahead of every target.
#
# Exceptions are native wasm exceptions: eacp has catch sites (Strings, Zip,
# Async), and Emscripten's default ignores them, turning every throw into an
# abort. Every object in a link must be built the same way, so the flag is
# global, not per target.
#
# ASYNCIFY is deliberately off: the loop is the browser's (see
# Core/Threads/EventLoop-Web.cpp), and nothing in eacp blocks the main thread.

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>)
add_link_options(-fwasm-exceptions)

# Emscripten ships no clang-scan-deps, and eacp uses no C++ modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# WebGPU through Dawn's webgpu.h, as Emscripten's own port provides it. Needed
# both to compile against the header and to link the JS library under it.
set(EACP_WEB_WEBGPU_OPTIONS --use-port=emdawnwebgpu)

set(EACP_WEB_SHELL "${CMAKE_CURRENT_LIST_DIR}/WebShell.html")

# Stack is 64 KiB by default, far below the native threads eacp is written for.
set(EACP_WEB_LINK_OPTIONS
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=1MB
        -sENVIRONMENT=web
        -sEXPORTED_RUNTIME_METHODS=addRunDependency,removeRunDependency
        ${EACP_WEB_WEBGPU_OPTIONS})

# An app is <target>.html beside its .js and .wasm, served together. The page
# is the shell's: one full-window canvas with id "canvas", which is what a
# Window's content view presents into, and a WebGPU device requested before
# main() runs (Module.preinitializedWebGPUDevice), since main() cannot wait on
# the promise.
function(eacp_web_app target)
    set_target_properties(${target} PROPERTIES SUFFIX ".html")
    target_link_options(${target} PRIVATE
            ${EACP_WEB_LINK_OPTIONS}
            "--shell-file=${EACP_WEB_SHELL}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${EACP_WEB_SHELL}")
endfunction()
