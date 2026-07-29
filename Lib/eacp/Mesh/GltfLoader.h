#pragma once

#include "MeshData.h"

#include <eacp/Core/Utils/FilePath.h>

#include <string>

// glTF 2.0 into MeshData, via cgltf.
//
// Both the .gltf (JSON plus separate buffers) and .glb (one binary container)
// forms load through the same call - cgltf detects which from the bytes - and a
// .gltf's external buffers and images are resolved relative to its own
// directory, which is why loadFromMemory needs a base path to do the same.
//
// What is deliberately not read yet, so that an absence reads as a decision:
// animation, skins, cameras, and every KHR_* extension. Each is either a second
// dependency or a phase of its own; see §5.8 of imgui-eacp's EACP_GPU_PLAN.md.

namespace eacp::Mesh
{
struct LoadResult
{
    MeshData data;

    // Empty when the load succeeded. A model that failed to load returns an
    // empty MeshData and the reason, rather than throwing - a malformed file is
    // an expected input for anything that opens what a user picked.
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

// Reads and parses a .gltf or .glb from disk.
LoadResult loadGltf(const FilePath& path);

// The same, from bytes already in memory. `basePath` is the directory external
// buffers and images are resolved against; a .glb with everything embedded needs
// none, and a .gltf without one loads its geometry and silently gets no
// textures.
LoadResult loadGltfFromMemory(const void* bytes,
                              std::size_t size,
                              const FilePath& basePath = {});
} // namespace eacp::Mesh
