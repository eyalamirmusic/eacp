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
// host looks it up with no mangled name spelled, and free to return a C++ type
// since module and app share one eacp built by one compiler (eacp_add_hot_reload
// silences clang's warning for that). Windows has no hot reload, so no module
// and the app calls its compiled-in copy directly; the macro is empty there,
// which is also why MSVC's refusal to return a class from a C function never
// applies.

#ifdef __cplusplus
#define EACP_PLUGIN_EXTERN_C extern "C"
#else
#define EACP_PLUGIN_EXTERN_C
#endif

#ifdef _WIN32
#define EACP_PLUGIN_EXPORT EACP_PLUGIN_EXTERN_C __declspec(dllexport)
#define EACP_HOT_RELOAD_EXPORT
#else
#define EACP_PLUGIN_EXPORT                                                          \
    EACP_PLUGIN_EXTERN_C __attribute__((visibility("default")))
#define EACP_HOT_RELOAD_EXPORT                                                      \
    EACP_PLUGIN_EXTERN_C __attribute__((visibility("default")))
#endif
