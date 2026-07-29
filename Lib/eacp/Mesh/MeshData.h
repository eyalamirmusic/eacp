#pragma once

#include "MeshTypes.h"

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Image/Image.h>

#include <string>

// The CPU side of a loaded model, named for what a renderer needs rather than
// for what any one file format calls things. Nothing here mentions glTF and no
// header here includes cgltf: GltfLoader is a translation *into* this, so a
// second loader is a new file rather than a refactor, and MeshRenderer never
// learns what produced its input.
//
// The seam is also what makes almost everything testable with no GPU - node
// composition, index packing and material defaults are all properties of this
// struct, and they are where the silent errors live.

namespace eacp::Mesh
{
// How a material's alpha is meant to be drawn. glTF's three modes, which are
// three different pipelines rather than three values in one.
enum class AlphaMode
{
    // Alpha is ignored entirely and the surface is opaque, whatever the base
    // colour's fourth channel says.
    Opaque,

    // A cutout: a fragment below the threshold is discarded, and what survives
    // is fully opaque. Still writes depth, so it sorts like opaque geometry -
    // which is why foliage and grates use it rather than Blend.
    Mask,

    // Ordinary alpha blending, which does not write depth and therefore has to
    // be drawn after everything opaque.
    Blend
};

struct Material
{
    std::string name;

    // glTF's defaults, not C++'s. A zero-initialised base colour is transparent
    // black and renders the entire model invisible, which reads as a loader that
    // did not run rather than one that ran and defaulted wrongly.
    float baseColor[4] {1.0f, 1.0f, 1.0f, 1.0f};

    // Index into MeshData::images, or -1 for none.
    int baseColorImage = -1;

    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;

    // Both faces rasterise. The renderer turns this into CullMode::None, and
    // its default of false into CullMode::Back.
    bool doubleSided = false;
};

// One draw: a contiguous index range into the model's single shared index
// buffer, plus the material to draw it with.
//
// firstIndex and baseVertex are what the GPU plan's Phase 1 exists for. Every
// primitive's indices start from zero and the offset into the shared vertex
// buffer is carried here instead of being added into the index values - which
// is what keeps them 16-bit on a model far past 65536 vertices, since no single
// primitive is near that even when the model is.
struct Primitive
{
    int firstIndex = 0;
    int indexCount = 0;
    int baseVertex = 0;

    // Index into MeshData::materials, or -1 for glTF's default material.
    int material = -1;

    Vec3 boundsMin;
    Vec3 boundsMax;
};

struct Mesh
{
    std::string name;
    Vector<int> primitives;
};

// A node in the scene hierarchy. `transform` is the node's own; `worldTransform`
// is that composed with every parent's, which the loader computes once because
// the hierarchy does not move in a static scene.
struct Node
{
    std::string name;

    Mat4 transform = Mat4::identity();
    Mat4 worldTransform = Mat4::identity();

    // Index into MeshData::meshes, or -1 for a node that only positions its
    // children.
    int mesh = -1;

    int parent = -1;
    Vector<int> children;
};

// A model: one vertex buffer's worth of geometry, one index buffer's worth, and
// the tree that says where each piece goes.
struct MeshData
{
    bool isValid() const { return vertices.size() > 0; }

    // Every node with a mesh, in the order they should be drawn. Flattened by
    // the loader so a renderer walks a list rather than recursing a tree every
    // frame.
    Vector<int> drawOrder;

    Vector<MeshVertex> vertices;

    // Kept wide here and narrowed at upload if it can be - see
    // fitsNarrowIndices. This is the format-agnostic scene, and how many bytes
    // an index costs on the GPU is not a property of the model.
    Vector<std::uint32_t> indices;

    Vector<Primitive> primitives;
    Vector<Mesh> meshes;
    Vector<Node> nodes;
    Vector<Material> materials;
    Vector<Graphics::Image> images;

    // The whole model's bounds in world space, which is what an app needs to
    // frame a camera on something it has never seen.
    Vec3 boundsMin;
    Vec3 boundsMax;

    Vec3 boundsCenter() const { return (boundsMin + boundsMax) * 0.5f; }

    // The largest edge of the bounding box - the scale a camera distance is
    // worked out from.
    float boundsExtent() const
    {
        auto size = boundsMax - boundsMin;
        return std::fmax(size.x, std::fmax(size.y, size.z));
    }
};

// Whether every index in the model fits in 16 bits, so the index buffer can be
// uploaded at half the width.
//
// It is a question about the largest *primitive* rather than about the model,
// and that is the whole payoff of the base vertex: indices are stored relative
// to their own primitive, so a model of two hundred thousand vertices still
// indexes narrowly as long as no single primitive passes 65536. A loader that
// rebased into the shared buffer instead would force 32-bit here on any model
// past that total, which is the cost §1.1 of the GPU plan set out to remove.
//
// It is not a given, though - one primitive genuinely can be larger than that -
// so this is checked rather than assumed, and the renderer uploads whichever
// width the answer calls for.
bool fitsNarrowIndices(const MeshData& data);
} // namespace eacp::Mesh
