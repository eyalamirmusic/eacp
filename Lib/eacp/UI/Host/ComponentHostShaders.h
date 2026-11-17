#pragma once

#include <eacp/GPU/Codegen/ShaderWarmUp.h>

namespace eacp::UI
{
// The shaders a ComponentHost draws its tree with that their renderers hand
// over - shapes, meshes, images, layers and glyphs - added to a warm-up, so the
// first host shown after it finishes compiles none of them. The clip, path and
// gradient programs are not among them yet and still compile on first use.
void addComponentHostShaders(GPU::ShaderWarmUp& warmUp);
} // namespace eacp::UI
