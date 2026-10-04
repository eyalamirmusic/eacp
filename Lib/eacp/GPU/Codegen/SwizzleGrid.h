// The cross product of the component set with itself, once per swizzle width.
// The action macro passed in is what makes one accessor out of a set of
// components: ShaderValue.h declares them inside Swizzles and ShaderValue.cpp
// defines them. Each includer undefines these again afterwards, which is why
// the file has no include guard.
#define EACP_SWIZZLE_PAIR_ROW(PAIR, a) PAIR(a, x) PAIR(a, y) PAIR(a, z) PAIR(a, w)

#define EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, b)                                    \
    TRIPLE(a, b, x) TRIPLE(a, b, y) TRIPLE(a, b, z) TRIPLE(a, b, w)

#define EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, a)                                          \
    EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, x)                                        \
    EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, y)                                        \
    EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, z)                                        \
    EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, w)

#define EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, c)                                    \
    QUAD(a, b, c, x) QUAD(a, b, c, y) QUAD(a, b, c, z) QUAD(a, b, c, w)

#define EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, b)                                        \
    EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, x)                                        \
    EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, y)                                        \
    EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, z)                                        \
    EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, w)

#define EACP_SWIZZLE_QUAD_ROW(QUAD, a)                                              \
    EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, x)                                            \
    EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, y)                                            \
    EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, z)                                            \
    EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, w)

// clang-format off
#define EACP_SWIZZLES(ONE, PAIR, TRIPLE, QUAD)                                      \
    ONE(x) ONE(y) ONE(z) ONE(w)                                                     \
    EACP_SWIZZLE_PAIR_ROW(PAIR, x)                                                  \
    EACP_SWIZZLE_PAIR_ROW(PAIR, y)                                                  \
    EACP_SWIZZLE_PAIR_ROW(PAIR, z)                                                  \
    EACP_SWIZZLE_PAIR_ROW(PAIR, w)                                                  \
    EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, x)                                              \
    EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, y)                                              \
    EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, z)                                              \
    EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, w)                                              \
    EACP_SWIZZLE_QUAD_ROW(QUAD, x)                                                  \
    EACP_SWIZZLE_QUAD_ROW(QUAD, y)                                                  \
    EACP_SWIZZLE_QUAD_ROW(QUAD, z)                                                  \
    EACP_SWIZZLE_QUAD_ROW(QUAD, w)
// clang-format on
