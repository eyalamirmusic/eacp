#pragma once

// Marks a C function as exported from an eacp plugin. The host resolves
// them with DynamicLibrary::findFunction after enumerating via
// getFunctionNames.
//
// The set of exported functions — and, when host and plugin ship
// independently, their versioning — is the app's own contract. eacp never
// crosses the boundary: host and plugin each statically link their own copy,
// so nothing with C++ layout may pass through these functions; stick to C
// primitives, C structs and function pointers.
//
// A hot-reload module (eacp_add_hot_reload) is the exception: it links nothing
// and binds at load time to the eacp the host exports, so C++ types cross
// freely. Its entry point is marked EACP_HOT_RELOAD_EXPORT: extern "C" so the
// host looks it up with no mangled name spelled. It returns a pointer to a C++
// type, which C linkage allows with no compiler warning (a class by value
// would draw clang's -Wreturn-type-c-linkage and MSVC's C4190), and which is
// safe since module and app share one eacp built by one compiler.
//
// On Windows the entry is exported only from a module: a target that defines
// EACP_HOT_RELOAD_MODULE (eacp_add_hot_reload sets it on its module in dev
// mode). Anywhere else, a static-mode app included, it is plain extern "C", so
// the executable exports nothing.

#ifdef __cplusplus
#define EACP_PLUGIN_EXTERN_C extern "C"
#else
#define EACP_PLUGIN_EXTERN_C
#endif

#ifdef _WIN32
#define EACP_PLUGIN_EXPORT EACP_PLUGIN_EXTERN_C __declspec(dllexport)
#ifdef EACP_HOT_RELOAD_MODULE
#define EACP_HOT_RELOAD_EXPORT EACP_PLUGIN_EXTERN_C __declspec(dllexport)
#else
#define EACP_HOT_RELOAD_EXPORT EACP_PLUGIN_EXTERN_C
#endif
#else
#define EACP_PLUGIN_EXPORT                                                          \
    EACP_PLUGIN_EXTERN_C __attribute__((visibility("default")))
#define EACP_HOT_RELOAD_EXPORT                                                      \
    EACP_PLUGIN_EXTERN_C __attribute__((visibility("default")))
#endif
