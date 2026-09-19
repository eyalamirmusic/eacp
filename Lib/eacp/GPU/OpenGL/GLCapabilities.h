#pragma once

#include "../Codegen/GlslLowering.h"

#include <string>

// One GL backend for every version: what the context can do, filled once when
// it comes up, and a flag per feature the code below branches on. No line under
// device creation compares a version number - the floor path is the default
// everywhere and every higher-version form is optional speed behind a flag
// here (plan.md D4).
namespace eacp::GPU
{
struct GLCapabilities
{
    // What the context answered, rather than what was asked for: a driver may
    // hand back more than the version requested, and virgl hands back GL 4.0
    // for a 3.3 request.
    bool isES = false;

    // 330, 400, 430, 460 on core; 300, 310, 320 on ES. The number the GLSL
    // target is picked from and nothing else reads.
    int version = 0;

    std::string renderer;
    std::string vendor;
    std::string versionString;
    std::string shadingLanguageVersion;

    // A compute stage at all: GL 4.3 / ES 3.1. Everything the kernel tier needs
    // comes with it, which is what the invariants below say.
    bool computeShaders = false;
    bool storageBuffers = false;
    bool imageLoadStore = false;

    // glBufferStorage, and so a persistent mapping for a streaming buffer. The
    // two spellings are one capability: core on desktop, GL_EXT_buffer_storage
    // on ES.
    bool bufferStorage = false;
    bool bufferStorageIsEXT = false;

    // glTexStorage2D, which fixes a texture's levels and format once rather
    // than a glTexImage2D per level.
    bool textureStorage = false;

    // GL_TIMESTAMP queries. The ES spelling is GL_EXT_disjoint_timer_query,
    // whose entry points are suffixed and whose results may be thrown away by
    // the driver, which is what GL_GPU_DISJOINT_EXT reports.
    bool timerQuery = false;
    bool timerQueryIsEXT = false;

    // layout(binding = N) on a block, a sampler or an image. The GL backend
    // binds by name after linking whether or not this is set (D4), so this is
    // read only to pick the GLSL target.
    bool explicitBindings = false;
    bool separateShaderObjects = false;

    bool bptc = false;
    bool s3tc = false;

    // glClipControl, which is what puts clip-space depth in the [0, 1] the
    // other three backends have rather than GL's own [-1, 1]. Absent on virgl.
    // It would also fix the y flip in one call, which the lowering wraps main
    // for instead (D7), because a context without it still has to be right.
    // The ES spelling is GL_EXT_clip_control, a suffixed entry point rather
    // than the same one under another name.
    bool clipControl = false;
    bool clipControlIsEXT = false;

    bool debugOutput = false;

    // GL_BGRA as the pair a sized GL_RGBA8 texture is uploaded and read back
    // through, which is desktop core and nowhere else: ES's
    // GL_EXT_texture_format_BGRA8888 is a different thing - an unsized
    // GL_BGRA_EXT internal format - so it does not answer this, and
    // BGRA8Unorm is stored RGBA behind a swizzle there with every row swapped
    // on the way through.
    bool bgraFormat = false;

    // glGetTexImage, which reads a texture that is not colour-renderable and
    // so cannot be attached to the read-back FBO. Desktop only; ES has none,
    // and such a texture simply does not read back there.
    bool getTexImage = false;

    // glGetBufferSubData, which is the read-back a buffer wants and which ES
    // does not have either: a map-read of the range is the route there.
    bool getBufferSubData = false;

    // Float and half-float colour attachments, which is what a render target
    // in one of those formats needs. Core on desktop; on ES these are
    // GL_EXT_color_buffer_float and GL_EXT_color_buffer_half_float.
    bool floatRenderTargets = false;
    bool halfFloatRenderTargets = false;

    int maxSamples = 0;
    int maxTextureSize = 0;
    int maxThreadgroupMemory = 0;

    // The grid glBindBufferRange's offset has to sit on, per target. The
    // Device reports the larger of the two as storageBufferOffsetAlignment,
    // since a caller that rounds to it satisfies both binds.
    int uniformBufferOffsetAlignment = 4;
    int storageBufferOffsetAlignment = 4;

    // The GLSL the lowering should produce for this context: the highest
    // target it knows that this context accepts.
    GlslTarget glslTarget() const
    {
        auto target = GlslTarget {};
        target.profile = isES ? GlslTarget::Profile::ES : GlslTarget::Profile::Core;

        if (isES)
            target.version = version >= 320 ? 320 : (version >= 310 ? 310 : 300);
        else if (version >= 460)
            target.version = 460;
        else if (version >= 430)
            target.version = 430;
        else if (version >= 400)
            target.version = 400;
        else
            target.version = 330;

        return target;
    }
};
} // namespace eacp::GPU
