#include "MeshShader.h"

namespace eacp::Mesh
{
MeshShader::MeshShader()
{
    baseColorTexture.sampling = meshSampling;
    compile();
}

void MeshShader::define()
{
    auto position = vertexInput(&MeshVertex::position);
    auto normal = vertexInput(&MeshVertex::normal);
    auto uv = vertexInput(&MeshVertex::uv);
    auto color = vertexInput(&MeshVertex::color);

    setPosition(modelViewProjection * float4(position, 1.0f));

    // The normal transform rather than the model matrix: under a non-uniform
    // scale the two differ, and using the model matrix tilts the normal off the
    // surface it belongs to. The w of 0 is what makes this a direction - a
    // translation must not move it.
    auto worldNormal =
        normalize((normalTransform * float4(normal.xyz(), 0.0f)).xyz());

    // The packed normal arrives as a Float4 because there is no three-component
    // packed format on both backends; its w carries nothing and is dropped here.
    auto shadedNormal = varying(worldNormal);
    auto texel = sample(baseColorTexture, varying(uv));

    // No branch on whether the material has a texture: a material without one is
    // bound a 1x1 opaque white texture instead, so this multiply is the identity
    // and the shader has one path.
    auto surface = baseColor * varying(color) * texel;

    auto lambert =
        max(dot(normalize(shadedNormal), normalize(lightDirection)), 0.0f);

    // The ambient floor is what keeps the unlit side of a model readable rather
    // than black, which matters more here than physical honesty: this is an
    // inspector, and geometry facing away from the camera still has to be
    // findable.
    auto lit = surface.xyz() * (lambert * 0.8f + 0.2f);

    auto asNormals = shadedNormal * 0.5f + 0.5f;

    auto shaded =
        select(shadingMode == (int) ShadingMode::Lambert, lit, surface.xyz());
    shaded = select(shadingMode == (int) ShadingMode::Normals, asNormals, shaded);

    // The mask test, written as a subtraction because discardBelow takes a
    // literal threshold and the cutoff is per material. A material that does not
    // mask sets the cutoff to zero, and nothing has an alpha below that.
    setDiscardBelow(surface.w() - alphaCutoff, 0.0f);

    setFragment(float4(shaded, surface.w()));
}
} // namespace eacp::Mesh
