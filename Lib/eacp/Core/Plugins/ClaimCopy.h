#pragma once

#include "PluginExport.h"

// Claims, for the process executable, the eacp copy compiled into a library it
// links: that copy then answers Platform::isStandalone() — it owns the event
// loop in Apps::run<T>() — and its product image (Files::applicationPath(), the
// Windows icon, the WebView2 data folder) is the executable. For a thin
// launcher whose application, eacp included, lives in a DLL, dylib or .so.
//
// One source of the executable, never a library's, says
//
//     EACP_CLAIM_COPY(myAppModuleAddress());
//
// where the expression is an address inside the library's image, normally a
// call to a function the library exports returning
// eacp::Plugins::getCurrentModuleAddress(). Call into the library rather than
// take &importedFunction: on Windows that address, without dllimport, is the
// import thunk in the executable. The expression runs as early as the
// library's first static initialiser, before the executable's own, so it must
// touch no static and nothing of the C runtime: a call into the library is
// exactly that.
//
// The executable's CMake target needs eacp_claim_copy(<target>)
// (CMake/EacpPlugin.cmake), which exports the claim on Linux; the
// static_assert below refuses to build without it on every platform. Only the
// copy the claim names is affected: any other library's copy, a plugin the
// launcher loads included, is still a guest. Docs/Build.md, "Who owns the
// process", is the account.

#ifndef EACP_CLAIM_COPY_LINKED
#define EACP_CLAIM_COPY_LINKED 0
#endif

#ifdef _WIN32
#define EACP_CLAIM_COPY_EXPORT __declspec(dllexport)
#else
#define EACP_CLAIM_COPY_EXPORT __attribute__((visibility("default"), used))
#endif

#define EACP_CLAIM_COPY(expression)                                                 \
    EACP_PLUGIN_EXTERN_C EACP_CLAIM_COPY_EXPORT const void* eacpClaimedCopy()       \
    {                                                                               \
        return (expression);                                                        \
    }                                                                               \
    static_assert(EACP_CLAIM_COPY_LINKED,                                           \
                  "EACP_CLAIM_COPY needs eacp_claim_copy(<target>) in CMake, "      \
                  "or the claim is not exported from the executable")
