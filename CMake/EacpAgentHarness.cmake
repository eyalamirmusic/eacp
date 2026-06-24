# Developer/agent affordances for eacp apps.
#
# Three independent affordances, each on by default in non-release
# builds, each with its own switch — mix them freely:
#
#   * The agent harness — a debugger-wrapped launcher at
#     <build>/agentharness/<Target>. Runs the app exactly like a direct
#     launch, but a crash prints a full backtrace and exits non-zero
#     where the agent can read it. Switch: EACP_AGENT_HARNESS.
#   * AddressSanitizer on the app executable, so memory bugs abort with
#     a precise report instead of corrupting silently. Switch:
#     EACP_ASAN. Orthogonal to the harness — enable it with or without.
#   * (WebView apps only) the embedded MCP debug server, so an agent can
#     list/click/type/screenshot the live UI over HTTP. Switch:
#     EACP_DEBUG_SERVER (defined alongside eacp_enable_debug_server).
#
# Each switch is AUTO | ON | OFF, resolved independently: AUTO means
# "developer builds only" — every configuration except an explicit
# release one (Release / RelWithDebInfo / MinSizeRel), so production
# binaries carry none of it. There is deliberately no master switch
# coupling them: EACP_AGENT_HARNESS=OFF EACP_ASAN=ON gives ASan with no
# launcher, and vice versa.
#
# eacp_add_webview_app() installs all three automatically (per-app
# opt-outs: NO_ASAN, NO_DEBUG_SERVER). For a hand-rolled executable:
#
#     add_executable(MyTool Main.cpp)
#     eacp_add_agent_harness(MyTool)   # launcher
#     eacp_enable_agent_asan(MyTool)   # and/or ASan, independently
#
# Each launcher also gets a build-and-run convenience target:
#     cmake --build build --target agentharness-MyTool

set(EACP_AGENT_HARNESS "AUTO" CACHE STRING
        "Debugger-wrapped launcher for app executables: AUTO (non-release builds), ON, OFF")
set_property(CACHE EACP_AGENT_HARNESS PROPERTY STRINGS AUTO ON OFF)

set(EACP_ASAN "AUTO" CACHE STRING
        "AddressSanitizer on app executables: AUTO (non-release builds), ON, OFF")
set_property(CACHE EACP_ASAN PROPERTY STRINGS AUTO ON OFF)

# Resolves AUTO/ON/OFF against the build type: ON -> TRUE, OFF ->
# FALSE, AUTO -> TRUE unless this is an explicit release configuration.
function(eacp_tristate_enabled SETTING OUT_VAR)
    if (SETTING STREQUAL "ON")
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    elseif (SETTING STREQUAL "OFF")
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    elseif (CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    else ()
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    endif ()
endfunction()

# AddressSanitizer for the binaries agents launch and drive. Applied to
# the executable target only — its own TUs get full instrumentation,
# and linking the ASan runtime into the process makes the allocator
# interceptors global, so heap bugs (use-after-free, double-free, heap
# overflow) are caught in the static libraries too. Stack/global
# overflow checks remain limited to instrumented TUs; rebuild with
# global -fsanitize=address flags when chasing one of those.
function(eacp_enable_agent_asan TARGET)
    eacp_tristate_enabled("${EACP_ASAN}" ENABLED)
    if (NOT ENABLED)
        return ()
    endif ()

    # Windows ASan (MSVC and clang-cl) is incompatible with the debug CRT
    # (/MDd) the Debug build uses — clang-cl rejects the combination
    # outright. Skip it there rather than forcing a different runtime
    # across the whole link; the agent harness launcher (cdb) is the
    # Windows crash path. Apple + Linux get full ASan.
    if (WIN32)
        return ()
    endif ()

    target_compile_options(${TARGET} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${TARGET} PRIVATE -fsanitize=address)
endfunction()

# Internal: generates the agent harness launcher for an executable
# target at a predictable path:
#
#     <build>/agentharness/<Target>          (single-config generators)
#     <build>/agentharness/<Config>/<Target> (multi-config generators)
#
# Behaves like running the binary directly — arguments, environment,
# stdout/stderr all pass through, so workflows like the MCP debug
# server are unaffected — but runs the process under a batch-mode
# debugger so a crash prints a full backtrace and exits non-zero.
#
# Debugger selection happens at launch, not configure: lldb, then gdb,
# then a plain exec on POSIX; cdb (Debugging Tools for Windows), then a
# plain start on Windows. Set EACP_NO_DEBUGGER=1 to bypass the debugger
# without changing the call site. Also defines an `agentharness-<Target>`
# convenience target (build + launch). Call eacp_add_agent_harness()
# rather than this directly.
function(_eacp_generate_launcher TARGET)
    if (NOT TARGET ${TARGET})
        message(FATAL_ERROR "eacp_add_agent_harness(${TARGET}): no such target")
    endif ()

    get_target_property(TARGET_TYPE ${TARGET} TYPE)
    if (NOT TARGET_TYPE STREQUAL "EXECUTABLE")
        message(FATAL_ERROR
                "eacp_add_agent_harness(${TARGET}): target is ${TARGET_TYPE}, "
                "expected EXECUTABLE")
    endif ()

    # Multi-config generators resolve $<TARGET_FILE> differently per
    # config, so each config gets its own script directory.
    if (CMAKE_CONFIGURATION_TYPES)
        set(HARNESS_DIR "${CMAKE_BINARY_DIR}/agentharness/$<CONFIG>")
    else ()
        set(HARNESS_DIR "${CMAKE_BINARY_DIR}/agentharness")
    endif ()

    if (WIN32)
        set(SCRIPT_PATH "${HARNESS_DIR}/${TARGET}.cmd")
        set(CONTENT "@echo off
rem Generated by the eacp agent harness for ${TARGET} - do not edit.
rem Runs the app under cdb when available so a crash prints stacks.
rem Set EACP_NO_DEBUGGER=1 to launch the binary directly.
set \"BINARY=$<TARGET_FILE:${TARGET}>\"

if defined EACP_NO_DEBUGGER goto direct

where cdb >nul 2>nul
if not %ERRORLEVEL%==0 goto direct

rem -g/-G skip the initial/final breakpoints, so the -c command only
rem runs when an exception stops the process: print stacks and quit.
cdb -g -G -c \"!uniqstack -p; q\" \"%BINARY%\" %*
exit /b %ERRORLEVEL%

:direct
\"%BINARY%\" %*
exit /b %ERRORLEVEL%
")
    else ()
        set(SCRIPT_PATH "${HARNESS_DIR}/${TARGET}")
        set(CONTENT "#!/bin/sh
# Generated by the eacp agent harness for ${TARGET} - do not edit.
# Runs the app under a batch-mode debugger: behaves like a plain
# launch (args, env, stdio pass through), but a crash prints a full
# backtrace and exits non-zero. Set EACP_NO_DEBUGGER=1 to launch the
# binary directly.
BINARY=\"$<TARGET_FILE:${TARGET}>\"

if [ -n \"$EACP_NO_DEBUGGER\" ]; then
    exec \"$BINARY\" \"$@\"
fi

if command -v lldb >/dev/null 2>&1; then
    exec lldb --batch -o run \\
        -k \"thread backtrace all\" -k \"quit 1\" -- \"$BINARY\" \"$@\"
fi

if command -v gdb >/dev/null 2>&1; then
    exec gdb --batch -ex run -ex \"thread apply all bt\" \\
        --args \"$BINARY\" \"$@\"
fi

exec \"$BINARY\" \"$@\"
")
    endif ()

    file(GENERATE
            OUTPUT "${SCRIPT_PATH}"
            CONTENT "${CONTENT}"
            TARGET ${TARGET}
            FILE_PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE)

    if (NOT TARGET agentharness-${TARGET})
        add_custom_target(agentharness-${TARGET}
                COMMAND "${SCRIPT_PATH}"
                DEPENDS ${TARGET}
                USES_TERMINAL
                COMMENT "Running ${TARGET} under the agent harness")

        # Group next to the target it runs in IDE target trees.
        get_target_property(TARGET_FOLDER ${TARGET} FOLDER)
        if (TARGET_FOLDER)
            set_target_properties(agentharness-${TARGET} PROPERTIES
                    FOLDER "${TARGET_FOLDER}")
        endif ()
    endif ()
endfunction()

# Installs the agent harness on an executable target: the
# debugger-wrapped launcher, gated by EACP_AGENT_HARNESS. AddressSanit-
# izer is a separate affordance — call eacp_enable_agent_asan()
# alongside this (or on its own) to add it.
function(eacp_add_agent_harness TARGET)
    eacp_tristate_enabled("${EACP_AGENT_HARNESS}" ENABLED)
    if (NOT ENABLED)
        return ()
    endif ()

    _eacp_generate_launcher(${TARGET})
endfunction()

# The embedded MCP debug/capture server — a window-level developer
# affordance: screenshot + screen recording over HTTP, for ANY app (GPU,
# native drawing, SVG, WebView), not just WebView ones. Sibling to the
# launcher (EACP_AGENT_HARNESS) and ASan (EACP_ASAN); AUTO means
# non-release builds, same as the others. The WebView DOM tools layer
# onto this same server when present (see eacp_enable_debug_server).
set(EACP_DEBUG_SERVER "AUTO" CACHE STRING
        "Embed the MCP debug/capture server in apps: AUTO (non-release builds), ON, OFF")
set_property(CACHE EACP_DEBUG_SERVER PROPERTY STRINGS AUTO ON OFF)

# Links the window debug/capture server into ${TARGET} and compiles its
# auto-attach registration TU in, so the app's primary window hosts an
# MCP server (screenshot + recording) at runtime. Port policy:
# Graphics/Remote/WindowAutoAttach.h (EACP_DEBUG_PORT).
function(eacp_enable_window_debug_server TARGET)
    if (NOT TARGET eacp-graphics-remote)
        return ()
    endif ()

    eacp_tristate_enabled("${EACP_DEBUG_SERVER}" ENABLED)
    if (NOT ENABLED)
        return ()
    endif ()

    get_target_property(REMOTE_DIR eacp-graphics-remote SOURCE_DIR)
    target_link_libraries(${TARGET} PRIVATE eacp-graphics-remote)
    target_sources(${TARGET} PRIVATE ${REMOTE_DIR}/WindowAutoAttachRegister.cpp)
endfunction()

# Code-signing identity for dev app bundles. Unset (default) leaves the
# linker-signed adhoc signature, whose designated requirement is a per-
# build cdhash — so any TCC consent (Screen Recording, Camera, …) is lost
# on the next rebuild, since macOS keys consent to the designated
# requirement. Set this to a stable identity (see
# `security find-identity -v -p codesigning`) and the requirement becomes
# identity-based, so the grant persists across rebuilds — exactly how a
# shipping app keeps its permissions. Local-dev convenience: never set in
# CI.
set(EACP_CODESIGN_IDENTITY "" CACHE STRING
        "Code-signing identity for dev app bundles (stable TCC grants); empty = adhoc")

# Re-signs ${TARGET} with EACP_CODESIGN_IDENTITY after each build, when one
# is configured. POST_BUILD, so it runs after the launcher/ASan have
# produced the final binary. No hardened runtime (it would demand
# entitlements WKWebView/ASan don't carry) — a plain signature is all TCC
# needs for a stable designated requirement.
function(eacp_codesign_app TARGET)
    if (NOT APPLE OR NOT EACP_CODESIGN_IDENTITY)
        return ()
    endif ()

    get_target_property(IS_BUNDLE ${TARGET} MACOSX_BUNDLE)
    if (IS_BUNDLE)
        set(SIGN_PATH "$<TARGET_BUNDLE_DIR:${TARGET}>")
    else ()
        set(SIGN_PATH "$<TARGET_FILE:${TARGET}>")
    endif ()

    add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND codesign --force --deep --timestamp=none
                    --sign "${EACP_CODESIGN_IDENTITY}" "${SIGN_PATH}"
            COMMENT
                "Code-signing ${TARGET} with '${EACP_CODESIGN_IDENTITY}' (stable TCC grants)"
            VERBATIM)
endfunction()

# The full dev-affordance set for a hand-rolled app target, in one call:
# the debugger-wrapped launcher, AddressSanitizer, the window debug/
# capture server, and (if configured) a stable code signature — each
# still independently gated by its own switch. eacp_add_webview_app wires
# this same set (plus the WebView DOM tools) automatically; plain
# executables opt in by calling this.
function(eacp_enable_dev_affordances TARGET)
    eacp_add_agent_harness(${TARGET})
    eacp_enable_agent_asan(${TARGET})
    eacp_enable_window_debug_server(${TARGET})
    eacp_codesign_app(${TARGET})
endfunction()
