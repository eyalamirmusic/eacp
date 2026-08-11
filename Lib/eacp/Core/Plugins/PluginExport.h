#pragma once

// Marks a C function as exported from an eacp plugin, resolved by the host with
// DynamicLibrary::findFunction. Host and plugin each statically link their own
// eacp, so only C primitives, C structs and function pointers may cross.

#ifdef __cplusplus
#define EACP_PLUGIN_EXTERN_C extern "C"
#else
#define EACP_PLUGIN_EXTERN_C
#endif

#ifdef _WIN32
#define EACP_PLUGIN_EXPORT EACP_PLUGIN_EXTERN_C __declspec(dllexport)
#else
#define EACP_PLUGIN_EXPORT                                                          \
    EACP_PLUGIN_EXTERN_C __attribute__((visibility("default")))
#endif
