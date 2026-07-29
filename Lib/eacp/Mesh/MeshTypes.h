#pragma once

#include <eacp/GPU/GPU.h>

#include <cmath>

// The small amount of maths a scene graph needs, and the vertex it feeds the
// GPU.
//
// eacp has no CPU matrix type, and that is not an oversight: the EDSL's
// translate / rotateX / perspective build a Float4x4 inside define() from
// scalar uniforms, which is the right shape for one object whose transform the
// shader can be written against. A glTF node's transform is composed down the
// hierarchy from its parents, and the hierarchy is data - so the composition
// happens here and arrives as a uniform.
//
// The convention matches the EDSL's exactly, because the two are multiplied
// together in the shader and nothing catches a mismatch but the picture:
// column-major storage, right-handed, depth mapped onto [0, 1].

namespace eacp::Mesh
{
struct Vec3
{
    using ShaderValue = GPU::Float3;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(Vec3 v, float scale)
{
    return {v.x * scale, v.y * scale, v.z * scale};
}

inline float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(Vec3 v)
{
    return std::sqrt(dot(v, v));
}

inline Vec3 normalize(Vec3 v)
{
    auto len = length(v);
    return len > 1.0e-8f ? v * (1.0f / len) : Vec3 {0.0f, 0.0f, 1.0f};
}

inline Vec3 min(Vec3 a, Vec3 b)
{
    return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)};
}

inline Vec3 max(Vec3 a, Vec3 b)
{
    return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)};
}

// A rotation as glTF stores one, xyzw with the scalar last. Kept as a quaternion
// rather than converted at load because a node's TRS is interpolated by
// animation, and slerp on a matrix is not a thing.
struct Quat
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

// Column-major: values[column * 4 + row], which is the layout both MSL and HLSL
// read a float4x4 out of a uniform block in, and the layout glTF writes a node
// matrix in. So a glTF matrix copies straight in and a uniform copies straight
// out, with no transpose anywhere - the one operation that is invisible when it
// is wrong on a symmetric matrix and catastrophic on every other.
struct Mat4
{
    using ShaderValue = GPU::Float4x4;

    static Mat4 identity();

    static Mat4 translation(Vec3 offset);
    static Mat4 scaling(Vec3 factors);
    static Mat4 rotation(Quat q);

    // The TRS a glTF node gives when it does not give a matrix, composed in
    // glTF's own order: translation * rotation * scale.
    static Mat4 fromTRS(Vec3 translationToUse, Quat rotationToUse, Vec3 scale);

    // Right-handed, y up, looking down -z, with depth in [0, 1] - the same
    // projection ShaderBuilder::perspective builds, written out here because
    // the camera lives on the CPU.
    static Mat4
        perspective(float aspect, float fovYRadians, float nearZ, float farZ);

    static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up);

    float& at(int column, int row) { return values[column * 4 + row]; }
    float at(int column, int row) const { return values[column * 4 + row]; }

    Vec3 transformPoint(Vec3 point) const;
    Vec3 transformDirection(Vec3 direction) const;

    // The determinant of the upper-left 3x3, whose sign says whether this
    // transform mirrors. A mirrored instance reverses the winding of every
    // triangle under it, so a renderer culling back faces has to flip its front
    // face for one or the model turns inside out.
    float linearDeterminant() const;

    // The inverse transpose of the upper-left 3x3, in the upper-left 3x3 of the
    // result. A normal transformed by the model matrix itself is wrong under
    // non-uniform scale - it stops being perpendicular to the surface, and the
    // shading bends with it.
    Mat4 normalMatrix() const;

    float values[16] {};
};

Mat4 operator*(const Mat4& a, const Mat4& b);

static_assert(sizeof(Mat4) == sizeof(float) * 16,
              "a Mat4 is uploaded as a Float4x4 uniform by memcpy");
static_assert(sizeof(Vec3) == sizeof(float) * 3,
              "a Vec3 is uploaded as a Float3 uniform by memcpy");

// What the pipeline consumes. Every packed format Phase 3 of the GPU plan landed
// is here except the two-component ones, and the vertex is 28 bytes against the
// 48 it would be with everything unpacked.
//
// The normal is four components with an unused w rather than an octahedral pair,
// which would be 24 bytes. Octahedral decoding is arithmetic in the shader, and
// the whole point of the packed formats is that both backends widen an attribute
// during the vertex fetch, in hardware, for free. Four bytes is the price of
// keeping that true, and it is the right one until vertex fetch is measured to
// be the limit.
struct MeshVertex
{
    float position[3];
    GPU::SNorm16x4 normal;
    GPU::Float16x2 uv;
    GPU::UNorm8x4 color;
};

static_assert(sizeof(MeshVertex) == 28, "the mesh vertex is meant to stay packed");
} // namespace eacp::Mesh
