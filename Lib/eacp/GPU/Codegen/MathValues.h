#pragma once

#include <eacp/Core/Maths/Maths.h>

#include "ShaderProgram.h"

// Teach the shader layer the shape of the Core maths types, so a Vec3 stands in
// wherever a Float3 vertex field or uniform is expected and a Mat4 wherever a
// Float4x4 is - which is what lets an app do its CPU-side geometry in the same
// types it feeds the GPU, with no repacking on the way across.
//
// Registered here rather than in Core, which knows nothing of shaders, and once
// rather than per app: two specialisations of the same type in one translation
// unit would not compile. Mirrors GPUWidgets/View/Vertices.h, which does the
// same for Graphics::Point and Graphics::Color.
EACP_SHADER_VALUE(eacp::Maths::Vec2, Float2)
EACP_SHADER_VALUE(eacp::Maths::Vec3, Float3)
EACP_SHADER_VALUE(eacp::Maths::Vec4, Float4)
EACP_SHADER_VALUE(eacp::Maths::Mat4, Float4x4)
