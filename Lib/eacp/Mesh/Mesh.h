#pragma once

// Umbrella header for the Mesh module: loading a model, and drawing one.
//
// The split is deliberate. MeshData is a format-agnostic scene an app can build,
// inspect and test with no GPU present; GltfLoader is one translation into it;
// MeshRenderer is the only part that needs a device. An app that only wants to
// read a model's contents includes MeshData.h and GltfLoader.h and links no
// pipeline at all.

#include "GltfLoader.h"
#include "MeshData.h"
#include "MeshRenderer.h"
#include "MeshShader.h"
#include "MeshTypes.h"
