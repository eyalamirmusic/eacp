#pragma once

#include "PluginExport.h"

// EACP_CLAIM_COPY(address), in one source of an executable whose CMake target
// calls eacp_claim_copy(), makes the eacp copy in the linked library holding
// `address` the application. The expression runs in the library's first static
// initialiser, before the executable's C runtime is up, so it must be a plain
// call into the library (one returning Plugins::getCurrentModuleAddress()),
// never &importedFunction, which on Windows is the executable's import thunk.
// Docs/Build.md, "Who owns the process".

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
