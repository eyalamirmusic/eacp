#pragma once

#include "MeshTypes.h"

// The model shader, written once in the EDSL so Metal and D3D12 cannot drift
// apart on it.
//
// Three shading modes rather than one, and two of them are for the inspector
// rather than for the picture. A model drawn flat looks the same whether its
// normals loaded correctly, arrived unnormalized, or were wrecked by the
// SNorm16x4 packing - so a mode that draws the normal directly is what makes the
// packing falsifiable by looking at it. The same instinct as comparing two
// renders rather than checking a colour against a constant: a picture that can
// only look right is not evidence.

namespace eacp::Mesh
{
enum class ShadingMode
{
    // Base colour only: the material factor times the base colour texture times
    // the vertex colour. What glTF's KHR_materials_unlit asks for.
    Unlit = 0,

    // A single light fixed to the camera, so nothing is ever in shadow and the
    // form still reads. The default, because an unlit model of one colour is a
    // silhouette.
    Lambert = 1,

    // The world-space normal as a colour. Not a shading model - a debug view,
    // and the one that says whether the normals in the file survived the trip.
    Normals = 2
};

// How every model texture is sampled. Repeat because a glTF UV outside [0, 1] is
// a tiled texture rather than an error, and Linear because a model is drawn at
// whatever size perspective makes it - which is also why the textures are
// created mipmapped.
constexpr GPU::TextureSampling meshSampling {.filter = GPU::TextureFilter::Linear,
                                             .addressMode =
                                                 GPU::TextureAddressMode::Repeat};

struct MeshShader final : GPU::ShaderProgram
{
    MeshShader();

    void define() override;

    GPU::Uniform<GPU::Float4x4> modelViewProjection;
    GPU::Uniform<GPU::Float4x4> normalTransform;
    GPU::Uniform<GPU::Float4> baseColor;

    // A light direction in world space. Fixed to the camera by the renderer, so
    // the lit side is always the side being looked at.
    GPU::Uniform<GPU::Float3> lightDirection;

    // Which ShadingMode to draw. A uniform rather than three compiled programs
    // because the whole point of the modes is switching between them while
    // looking at a model.
    GPU::Uniform<GPU::Int> shadingMode;

    // Below this the fragment is thrown away. Set to zero for a material that
    // does not mask, which discards nothing.
    GPU::Uniform<GPU::Float> alphaCutoff;

    // Always bound. A material with no base colour texture gets a 1x1 opaque
    // white one, so the shader multiplies by the identity rather than branching
    // - see define().
    GPU::Uniform<GPU::Texture2D> baseColorTexture;

    EACP_SHADER(modelViewProjection,
                normalTransform,
                baseColor,
                lightDirection,
                shadingMode,
                alphaCutoff,
                baseColorTexture)
};
} // namespace eacp::Mesh
