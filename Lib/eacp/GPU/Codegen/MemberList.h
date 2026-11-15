#pragma once

// Member-list reflection in the Miro idiom: list the declared members once and
// a body is generated that hands each to a visitor by name. Mirrors Miro's
// MIRO_FOR_EACH macro engine, kept GPU-local so the module needs no
// serialization dependency. EACP_SHADER hands a kernel's members to its
// visitor, EACP_SHADER_STRUCT a shader struct's member pointers to its own.
#define EACP_GPU_PARENS ()

#define EACP_GPU_EXPAND(...)                                                        \
    EACP_GPU_EXPAND3(EACP_GPU_EXPAND3(EACP_GPU_EXPAND3(__VA_ARGS__)))
#define EACP_GPU_EXPAND3(...)                                                       \
    EACP_GPU_EXPAND2(EACP_GPU_EXPAND2(EACP_GPU_EXPAND2(__VA_ARGS__)))
#define EACP_GPU_EXPAND2(...)                                                       \
    EACP_GPU_EXPAND1(EACP_GPU_EXPAND1(EACP_GPU_EXPAND1(__VA_ARGS__)))
#define EACP_GPU_EXPAND1(...) __VA_ARGS__

#define EACP_GPU_VISIT_FIELD(visitor, field) visitor(#field, field);
#define EACP_GPU_FIELDS_HELPER(visitor, a, ...)                                     \
    EACP_GPU_VISIT_FIELD(visitor, a)                                                \
    __VA_OPT__(EACP_GPU_FIELDS_AGAIN EACP_GPU_PARENS(visitor, __VA_ARGS__))
#define EACP_GPU_FIELDS_AGAIN() EACP_GPU_FIELDS_HELPER
#define EACP_GPU_FIELDS(visitor, ...)                                               \
    __VA_OPT__(EACP_GPU_EXPAND(EACP_GPU_FIELDS_HELPER(visitor, __VA_ARGS__)))

#define EACP_GPU_VISIT_MEMBER(visitor, field)                                       \
    visitor(#field, &ShaderStructSelf::field);
#define EACP_GPU_MEMBERS_HELPER(visitor, a, ...)                                    \
    EACP_GPU_VISIT_MEMBER(visitor, a)                                               \
    __VA_OPT__(EACP_GPU_MEMBERS_AGAIN EACP_GPU_PARENS(visitor, __VA_ARGS__))
#define EACP_GPU_MEMBERS_AGAIN() EACP_GPU_MEMBERS_HELPER
#define EACP_GPU_MEMBERS(visitor, ...)                                              \
    __VA_OPT__(EACP_GPU_EXPAND(EACP_GPU_MEMBERS_HELPER(visitor, __VA_ARGS__)))
