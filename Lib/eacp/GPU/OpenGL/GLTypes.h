#pragma once

#include "../Codegen/GlslLowering.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"
#include "../Timing/GpuTimestamps.h"

#include <glad/gl.h>

// The native structs the GL backend's void* nativeX() handles point at. Every
// cast of one stays inside a -GL.cpp file, exactly as the Vulkan backend's
// casts stay inside its own.
namespace eacp::GPU
{
// A TextureFormat as GL spells it: the sized internal format a texture is
// created with, and the pair an upload or a read-back is phrased in. A
// compressed format has no pair at all, its uploads going through
// glCompressedTexSubImage2D.
struct GLFormat
{
    bool isValid() const { return internalFormat != 0; }

    GLenum internalFormat = 0;
    GLenum format = 0;
    GLenum type = 0;
    bool compressed = false;

    // BGRA8Unorm on a profile with no GL_BGRA: stored as GL_RGBA8 with every
    // upload row swapped on the CPU and every read-back row swapped back, so
    // the texels sit in the channels their names say and a sample - or a
    // fragment written into it as a render target - means what it means
    // everywhere else. No GL_TEXTURE_SWIZZLE_* beside it: the swizzle is the
    // other way of doing this, not a second half of it, and applying both
    // reverses a sample.
    bool swappedBGRA = false;
};

struct GLBufferData
{
    bool isValid() const { return buffer != 0; }

    GLuint buffer = 0;
    std::int64_t size = 0;

    // A persistent mapping, under GLCapabilities::bufferStorage and
    // BufferStorage::Streaming alone. Null everywhere else, which is what
    // sends a write through glBufferSubData.
    std::byte* mapped = nullptr;
};

struct GLTextureData
{
    bool isValid() const { return texture != 0; }

    // GL_TEXTURE_2D, or GL_TEXTURE_CUBE_MAP for the six-faced kind.
    GLenum target = GL_TEXTURE_2D;

    GLuint texture = 0;

    int width = 0;
    int height = 0;
    int mipLevels = 1;
    int sampleCount = 1;

    bool cube = false;
    bool renderTarget = false;
    bool computeWrite = false;
    bool depth = false;
    bool stencil = false;
    bool sampleableDepth = false;

    TextureFormat format = TextureFormat::RGBA8Unorm;
    GLFormat gl;

    // The framebuffer a pass draws into and a read-back reads out of, made the
    // first time one is wanted rather than beside every render target.
    GLuint framebuffer = 0;

    // The multisampled colour a pass renders into, resolved into `texture` at
    // the end of it, so what is sampled and read back is always the resolved
    // picture. Its framebuffer is the one a pass binds; `framebuffer` above is
    // then the resolve destination.
    GLuint msaaColor = 0;
    GLuint msaaFramebuffer = 0;

    // GL_DEPTH24_STENCIL8 (or GL_DEPTH_COMPONENT24 with no stencil plane) as a
    // renderbuffer, or as a texture where the target asked for sampleableDepth
    // and a later pass will read it.
    GLuint depthRenderbuffer = 0;
    GLuint depthTexture = 0;

    // The single-sampled depth a multisampled sampleable-depth target resolves
    // into, which is what setFragmentDepthTexture binds there.
    GLuint resolvedDepthTexture = 0;
    GLuint resolvedDepthFramebuffer = 0;
};

struct GLShaderLibraryData
{
    // Compiled shader objects, kept rather than linked: a RenderPipeline links
    // its own program out of them, since the pipeline is what decides the
    // vertex layout and the state around it.
    GLuint vertex = 0;
    GLuint fragment = 0;
    GLuint compute = 0;

    // Every uniform block, storage block, sampler and image the source named,
    // by the name the linked program knows it by and the binding the Vulkan
    // source gave it. The pipeline binds these by name after linking, which is
    // the only route below core 420 / ES 310 and costs nothing above it (D4).
    Vector<NamedBinding> bindings;

    // What the sources were lowered for, so a pipeline can say so when a link
    // fails.
    GlslTarget target;
};

// The GL state a RenderPipeline carries beside its program: the descriptor's
// own answers, applied by diffing against what the pass last applied, so a
// pipeline switch costs only the calls that change (D6).
struct GLPipelineState
{
    PrimitiveTopology topology = PrimitiveTopology::Triangles;

    BlendState blend;
    ColorWriteMask colorWriteMask;

    bool depth = false;
    DepthCompare depthCompare = DepthCompare::LessEqual;
    bool depthWrite = true;

    bool stencil = false;
    StencilFace stencilFront;
    StencilFace stencilBack;
    unsigned char stencilReadMask = 0xff;
    unsigned char stencilWriteMask = 0xff;

    CullMode cullMode = CullMode::None;
    Winding frontFace = Winding::CounterClockwise;

    int sampleCount = 1;
};

// One binding the pipeline resolved at link time: the block index or the
// uniform location the name turned into, and the unit or binding point it is
// pointed at.
struct GLResolvedBinding
{
    // The uniform-block index, or the sampler/image uniform's location.
    GLint location = -1;

    // The binding point the Vulkan source named, which is the unit a texture
    // or a buffer is bound to at that slot.
    int binding = 0;
};

struct GLRenderPipelineData
{
    bool isValid() const { return program != 0; }

    GLuint program = 0;

    GLPipelineState state;
    VertexLayout vertexLayout;

    // The uniform blocks and samplers bound by name after linking, and the
    // location of the D7 y-flip sign the vertex wrapper reads. A pass sets the
    // sign to -1 on a texture target and +1 on the drawable.
    Vector<GLResolvedBinding> uniformBlocks;
    Vector<GLResolvedBinding> samplers;
    Vector<GLResolvedBinding> storageBlocks;
    GLint clipYSignLocation = -1;

    // What std140 made of the uniform block, which is the least a
    // glBindBufferRange may offer it however few bytes setBytes was handed.
    int uniformBlockSize = 0;
};

// One frame slot's timestamp queries: two per timed pass, then the frame's own
// pair, in the slot layout the Vulkan backend's query pool uses. A pass writes
// into it through GpuTimestamps::nativeSamples, which hands back one of these.
struct GLTimestampSlot
{
    static constexpr int frameStartQuery = GpuTimestamps::maxTimedPasses * 2;
    static constexpr int frameEndQuery = frameStartQuery + 1;
    static constexpr int queryCount = frameEndQuery + 1;

    // glQueryCounter on this index's query object, through whichever of the
    // two spellings the context has. A no-op where timing never came up.
    void writeTimestamp(int index) const;

    Array<GLuint, queryCount> queries {};
    bool useEXT = false;
    bool created = false;
};

// The render half's own three, which meet the resource half above at this
// header rather than negotiating a seam of their own (D6 as built).

// A Frame is the target it draws into, the uniform ring every draw on it writes
// through, and the size that target reports.
struct GLFrameData
{
    // Where a uniform block written by setBytes landed, which is what
    // glBindBufferRange takes.
    struct Range
    {
        bool isValid() const { return buffer != 0 && bytes > 0; }

        GLuint buffer = 0;
        GLintptr offset = 0;
        GLsizeiptr bytes = 0;
    };

    // Writes `bytes` of `data` into the ring at the next aligned offset,
    // padding up to `leastBytes` so a block declared larger than the caller
    // handed over is still wholly covered. Defined in Frame-GL.cpp.
    Range writeUniforms(const void* data, int bytes, int leastBytes);

    void releaseRing();

    int width = 0;
    int height = 0;

    GLuint uniformRing = 0;
    GLsizeiptr ringCapacity = 0;
    GLintptr ringCursor = 0;
    int ringAlignment = 4;
};

// What a presenting view hands a Frame: where on the view's own surface the
// pass draws, and how what it drew reaches the screen. The surface itself is an
// EGLSurface the view owns and nothing here names, this being the one struct
// the Frame and the pass read (GPUView-GL.cpp).
struct GLDrawable
{
    // The view's companion framebuffer where it has one - a multisampled
    // colour, a depth plane, or both - and the default framebuffer of the
    // current surface otherwise, which is what a plain view draws straight
    // into.
    GLuint framebuffer = 0;

    // Whether that companion is blitted into the default framebuffer at the end
    // of the pass; a multisampled one resolves in the same blit.
    bool resolveToDefault = false;

    int width = 0;
    int height = 0;

    // What the companion carries, which is what a pipeline built for a sample
    // count is matched against.
    int samples = 1;
    bool depth = false;
    bool stencil = false;

    // Presents what the frame drew, as the Frame goes: eglSwapBuffers on the
    // view's surface.
    std::function<void()> present = [] {};
};

// The open render pass, handed from the Frame to the RenderPass: what it draws
// into, what it resolves at the end, and the y sign this target wants. The
// last-applied state the next setPipeline diffs against belongs to the pass
// itself and lives in RenderPass-GL.cpp.
struct GLRenderEncoder
{
    Device* device = nullptr;
    GLFrameData* frame = nullptr;

    // Null on a drawable, which has no texture to resolve into.
    GLTextureData* target = nullptr;

    GLuint framebuffer = 0;

    int width = 0;
    int height = 0;

    // What the target takes, which a pipeline built for another count is
    // refused against. The texture's own on a texture target; the view's
    // companion on a drawable.
    int samples = 1;

    // A drawable's companion is blitted into the default framebuffer when the
    // pass ends, which is where a multisampled one resolves.
    bool resolveToDefault = false;

    // -1 on a texture, where the lowering's wrapper turns eacp's clip space
    // the right way up for an FBO, and +1 on a drawable (D7).
    float clipYSign = 1.f;

    // The slot a labelled pass writes its two timestamps into, and the index of
    // the second one. Null and -1 on a pass nobody asked to time.
    const GLTimestampSlot* timing = nullptr;
    int endQuery = -1;
};

// A CommandBuffer's recording, which on GL is the context's own stream plus the
// fence its wait() blocks on.
struct GLCommandEncoder
{
    GLsync fence = nullptr;
    bool committed = false;
};
} // namespace eacp::GPU
