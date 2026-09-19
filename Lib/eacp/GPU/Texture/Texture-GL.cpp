#include "Texture.h"

#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"
#include "MipChain.h"

#include <eacp/Core/Utils/Logging.h>

#include <cstring>
#include <vector>

namespace eacp::GPU
{
namespace
{
// eacp's row 0 is the top of the picture and GL's row 0 is the one it calls
// y = 0, so an upload in image order lands with the top row at y = 0 and every
// coordinate below is GL's and eacp's at once. What that costs is the third
// half of the rule - NDC +1 is the top - which the lowering's y-flip wrapper
// pays for at the vertex stage (plan.md D7), so nothing here flips anything.

constexpr int glFaceCount = 6;

constexpr GLenum glCubeFace(int face)
{
    return (GLenum) (GL_TEXTURE_CUBE_MAP_POSITIVE_X + face);
}

// The six faces in the +X, -X, +Y, -Y, +Z, -Z order the descriptor states,
// which is GL_TEXTURE_CUBE_MAP_POSITIVE_X + i exactly.
GLenum glUploadTarget(const GLTextureData& data, int face)
{
    return data.cube ? glCubeFace(face) : GL_TEXTURE_2D;
}

// A render target's colour attachment has to be attachable, and on ES a float
// one is only where its extension is.
bool glFormatIsRenderable(TextureFormat format, const GLCapabilities& caps)
{
    switch (format)
    {
        case TextureFormat::RGBA16Float:
            return caps.halfFloatRenderTargets;

        case TextureFormat::RGBA32Float:
        case TextureFormat::R32Float:
            return caps.floatRenderTargets;

        case TextureFormat::RGBA8Unorm:
        case TextureFormat::BGRA8Unorm:
        case TextureFormat::R8Unorm:
        case TextureFormat::RG8Unorm:
            return true;

        case TextureFormat::BC1RGBA:
        case TextureFormat::BC2RGBA:
        case TextureFormat::BC3RGBA:
        case TextureFormat::BC7RGBA:
            break;
    }

    return false;
}

struct GLTextureBackend final : TextureBackend
{
    GLTextureBackend(Device& device,
                     const TextureDescriptor& descriptor,
                     const void* pixels)
        : context(getGLContext(device))
    {
        data.width = descriptor.width;
        data.height = descriptor.height;
        data.cube = descriptor.cube;
        data.format = descriptor.format;
        data.target = descriptor.cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;

        if (!context.isValid() || data.width <= 0 || data.height <= 0)
            return;

        data.gl = glFormatFor(descriptor.format, context.getCapabilities());

        if (!descriptorIsPossible(device, descriptor, pixels))
            return;

        context.makeCurrent();

        // Everything below decides whether this texture exists by asking
        // glGetError, so the queue starts empty rather than holding whatever
        // was raised before this call.
        glForgetErrors();

        if (!createTexture() || !createCompanions())
        {
            release();
            return;
        }

        if (pixels != nullptr && !upload(pixels, 0))
        {
            release();
            return;
        }

        glDrainErrors("texture creation");
    }

    // The wrap constructor: there is no zero-copy pixel-buffer path on Linux,
    // so the texture comes out invalid and Texture::update is the route.
    GLTextureBackend(Device& device, void*)
        : context(getGLContext(device))
    {
    }

    ~GLTextureBackend() override { release(); }

    bool descriptorIsPossible(Device& device,
                              const TextureDescriptor& descriptor,
                              const void* pixels)
    {
        const auto& caps = context.getCapabilities();

        if (!data.gl.isValid())
        {
            LOG("OpenGL: this context has no ",
                data.gl.compressed ? "block-compressed" : "such",
                " format, so the texture is invalid rather than stored as "
                "something else");
            return false;
        }

        if (data.cube
            && (data.width != data.height || descriptor.renderTarget
                || descriptor.computeWrite))
            return false;

        if (data.width > caps.maxTextureSize || data.height > caps.maxTextureSize)
            return false;

        if (descriptor.renderTarget)
        {
            if (!glFormatIsRenderable(descriptor.format, caps))
            {
                LOG("OpenGL: this context cannot attach that format, so the "
                    "render target is invalid rather than never drawn into");
                return false;
            }

            data.renderTarget = true;
        }

        if (descriptor.renderTarget && descriptor.sampleCount > 1)
        {
            if (data.cube || !device.supportsSampleCount(descriptor.sampleCount))
            {
                LOG("OpenGL: the device refuses ",
                    descriptor.sampleCount,
                    " samples, so the target is invalid rather than drawn at "
                    "a count its pipelines do not carry");
                return false;
            }

            data.sampleCount = descriptor.sampleCount;
        }

        if (isCompressedFormat(data.format))
        {
            if (descriptor.renderTarget || descriptor.computeWrite)
                return false;

            if (!device.supportsBlockCompression())
            {
                LOG("OpenGL: the device has no block-compressed formats, so a "
                    "compressed texture is invalid here");
                return false;
            }
        }

        if (descriptor.computeWrite)
        {
            if (!supportsComputeWrite(descriptor.format) || !caps.imageLoadStore)
            {
                LOG("OpenGL: this context has no image store for that format, "
                    "so a computeWrite texture is invalid rather than silently "
                    "unwritable");
                return false;
            }

            data.computeWrite = true;
        }

        if (descriptor.mipLevels < 0)
            return false;

        if (descriptor.mipLevels > 0)
        {
            if (descriptor.mipmapped || descriptor.renderTarget
                || descriptor.computeWrite || data.cube || pixels == nullptr
                || descriptor.mipLevels > mipLevelCount(data.width, data.height))
                return false;

            data.mipLevels = descriptor.mipLevels;
            suppliedChain = true;
        }
        else if (descriptor.mipmapped && pixels != nullptr
                 && canBuildMipChain(data.format))
        {
            data.mipLevels = mipLevelCount(data.width, data.height);
        }

        if (data.renderTarget
            && (descriptor.depth || descriptor.stencil
                || descriptor.sampleableDepth))
        {
            data.depth = true;
            data.stencil = descriptor.stencil;
            data.sampleableDepth = descriptor.sampleableDepth;
        }

        return true;
    }

    bool createTexture()
    {
        glGenTextures(1, &data.texture);

        if (data.texture == 0)
            return false;

        glBindTexture(data.target, data.texture);

        glTexParameteri(data.target, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(data.target, GL_TEXTURE_MAX_LEVEL, data.mipLevels - 1);

        const auto stored = context.getCapabilities().textureStorage
                                ? allocateWithStorage()
                                : allocateWithImages();

        glBindTexture(data.target, 0);

        return stored;
    }

    bool allocateWithStorage()
    {
        glTexStorage2D(data.target,
                       (GLsizei) data.mipLevels,
                       data.gl.internalFormat,
                       (GLsizei) data.width,
                       (GLsizei) data.height);

        immutable = glGetError() == GL_NO_ERROR;

        return immutable;
    }

    // The floor path: one glTexImage2D per level per face, with no pixels, so
    // every level exists before the first upload names one.
    bool allocateWithImages()
    {
        if (data.gl.compressed)
        {
            // A compressed level cannot be declared without its blocks, so the
            // upload declares it; there is nothing to do here.
            return true;
        }

        const auto faces = data.cube ? glFaceCount : 1;

        for (auto face = 0; face < faces; ++face)
            for (auto level = 0; level < data.mipLevels; ++level)
                glTexImage2D(glUploadTarget(data, face),
                             (GLint) level,
                             (GLint) data.gl.internalFormat,
                             (GLsizei) mipExtent(data.width, level),
                             (GLsizei) mipExtent(data.height, level),
                             0,
                             data.gl.format,
                             data.gl.type,
                             nullptr);

        return glGetError() == GL_NO_ERROR;
    }

    bool createCompanions()
    {
        if (!data.renderTarget)
            return true;

        if (data.sampleCount > 1 && !createMultisampleCompanion())
            return false;

        return !data.depth || createDepthCompanion();
    }

    bool createMultisampleCompanion()
    {
        glGenRenderbuffers(1, &data.msaaColor);
        glBindRenderbuffer(GL_RENDERBUFFER, data.msaaColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER,
                                         (GLsizei) data.sampleCount,
                                         data.gl.internalFormat,
                                         (GLsizei) data.width,
                                         (GLsizei) data.height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        return glGetError() == GL_NO_ERROR;
    }

    // One combined attachment, as both the other backends have: asking for a
    // stencil plane gets the depth one with it, and a target the shader will
    // read gets a texture rather than a renderbuffer.
    bool createDepthCompanion()
    {
        const auto internalFormat =
            data.stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24;

        const auto multisampled = data.sampleCount > 1;

        if (data.sampleableDepth && !multisampled)
            return createDepthTexture(data.depthTexture, internalFormat);

        glGenRenderbuffers(1, &data.depthRenderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, data.depthRenderbuffer);

        if (multisampled)
            glRenderbufferStorageMultisample(GL_RENDERBUFFER,
                                             (GLsizei) data.sampleCount,
                                             (GLenum) internalFormat,
                                             (GLsizei) data.width,
                                             (GLsizei) data.height);
        else
            glRenderbufferStorage(GL_RENDERBUFFER,
                                  (GLenum) internalFormat,
                                  (GLsizei) data.width,
                                  (GLsizei) data.height);

        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        if (glGetError() != GL_NO_ERROR)
            return false;

        // The multisampled attachment resolves into this one, which is what
        // setFragmentDepthTexture binds there.
        if (data.sampleableDepth)
            return createDepthTexture(data.resolvedDepthTexture, internalFormat);

        return true;
    }

    bool createDepthTexture(GLuint& texture, int internalFormat)
    {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        if (context.getCapabilities().textureStorage)
            glTexStorage2D(GL_TEXTURE_2D,
                           1,
                           (GLenum) internalFormat,
                           (GLsizei) data.width,
                           (GLsizei) data.height);
        else
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         internalFormat,
                         (GLsizei) data.width,
                         (GLsizei) data.height,
                         0,
                         data.stencil ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
                         data.stencil ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_INT,
                         nullptr);

        glBindTexture(GL_TEXTURE_2D, 0);

        return glGetError() == GL_NO_ERROR;
    }

    void release()
    {
        if (!context.isValid())
            return;

        context.makeCurrent();

        for (auto* framebuffer: {&data.framebuffer,
                                 &data.msaaFramebuffer,
                                 &data.resolvedDepthFramebuffer})
            if (*framebuffer != 0)
                glDeleteFramebuffers(1, framebuffer);

        for (auto* renderbuffer: {&data.msaaColor, &data.depthRenderbuffer})
            if (*renderbuffer != 0)
                glDeleteRenderbuffers(1, renderbuffer);

        for (auto* texture:
             {&data.texture, &data.depthTexture, &data.resolvedDepthTexture})
            if (*texture != 0)
                glDeleteTextures(1, texture);

        data = {};
    }

    // Rows arrive in image order at the caller's stride; GL_UNPACK_ROW_LENGTH
    // takes that stride in texels, which is why a compressed upload - whose
    // rows are blocks - never passes one.
    void beginUnpack(int sourcePitch, int regionWidth) const
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        const auto tight = levelBytesPerRow(data.format, regionWidth);

        if (!data.gl.compressed && sourcePitch != tight
            && bytesPerPixel(data.format) > 0)
            glPixelStorei(GL_UNPACK_ROW_LENGTH,
                          sourcePitch / bytesPerPixel(data.format));
    }

    void endUnpack() const { glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); }

    // The swapped-BGRA route: the texels go up RGBA, so the caller's rows are
    // repacked with the two channels swapped rather than uploaded as they lie.
    const void* swapped(const void* pixels,
                        int sourcePitch,
                        int regionWidth,
                        int regionHeight)
    {
        const auto rowBytes =
            (std::size_t) levelBytesPerRow(data.format, regionWidth);
        const auto rows = (std::size_t) regionHeight;

        scratch.resize(rowBytes * rows);

        const auto* in = static_cast<const std::byte*>(pixels);

        for (auto row = std::size_t {0}; row < rows; ++row)
            std::memcpy(scratch.data() + row * rowBytes,
                        in + row * (std::size_t) sourcePitch,
                        rowBytes);

        glSwapRedAndBlue(
            scratch.data(), regionWidth, regionHeight, (int) rowBytes);

        return scratch.data();
    }

    void uploadLevel(const void* pixels,
                     int sourcePitch,
                     int destX,
                     int destY,
                     int regionWidth,
                     int regionHeight,
                     int level,
                     int face)
    {
        const auto target = glUploadTarget(data, face);

        if (data.gl.compressed)
        {
            const auto bytes =
                (GLsizei) levelBytes(data.format, regionWidth, regionHeight);

            if (immutable)
                glCompressedTexSubImage2D(target,
                                          (GLint) level,
                                          (GLint) destX,
                                          (GLint) destY,
                                          (GLsizei) regionWidth,
                                          (GLsizei) regionHeight,
                                          data.gl.internalFormat,
                                          bytes,
                                          pixels);
            else
                glCompressedTexImage2D(target,
                                       (GLint) level,
                                       data.gl.internalFormat,
                                       (GLsizei) regionWidth,
                                       (GLsizei) regionHeight,
                                       0,
                                       bytes,
                                       pixels);
            return;
        }

        auto pitch = sourcePitch;
        const auto* source = pixels;

        if (data.gl.swappedBGRA)
        {
            source = swapped(pixels, sourcePitch, regionWidth, regionHeight);
            pitch = levelBytesPerRow(data.format, regionWidth);
        }

        beginUnpack(pitch, regionWidth);

        glTexSubImage2D(target,
                        (GLint) level,
                        (GLint) destX,
                        (GLint) destY,
                        (GLsizei) regionWidth,
                        (GLsizei) regionHeight,
                        data.gl.format,
                        data.gl.type,
                        source);

        endUnpack();
    }

    void uploadFace(const void* pixels, int sourcePitch, int face)
    {
        if (data.mipLevels <= 1)
        {
            uploadLevel(
                pixels, sourcePitch, 0, 0, data.width, data.height, 0, face);
            return;
        }

        if (suppliedChain)
        {
            uploadPackedChain(pixels, face);
            return;
        }

        const auto chain =
            buildMipChain(pixels, data.width, data.height, data.format, sourcePitch);

        if (!chain.isValid())
            return;

        for (auto level = 0; level < data.mipLevels; ++level)
        {
            const auto levelWidth = mipExtent(data.width, level);
            const auto levelHeight = mipExtent(data.height, level);

            uploadLevel(chain.level(level),
                        levelBytesPerRow(data.format, levelWidth),
                        0,
                        0,
                        levelWidth,
                        levelHeight,
                        level,
                        face);
        }
    }

    void uploadPackedChain(const void* pixels, int face)
    {
        const auto* bytes = static_cast<const std::byte*>(pixels);

        for (auto level = 0; level < data.mipLevels; ++level)
        {
            const auto levelWidth = mipExtent(data.width, level);
            const auto levelHeight = mipExtent(data.height, level);

            uploadLevel(bytes,
                        levelBytesPerRow(data.format, levelWidth),
                        0,
                        0,
                        levelWidth,
                        levelHeight,
                        level,
                        face);

            bytes += levelBytes(data.format, levelWidth, levelHeight);
        }
    }

    bool upload(const void* pixels, int bytesPerRow)
    {
        const auto pitch = bytesPerRow != 0
                               ? bytesPerRow
                               : levelBytesPerRow(data.format, data.width);

        glBindTexture(data.target, data.texture);

        const auto faces = data.cube ? glFaceCount : 1;
        const auto faceBytes = (std::size_t) pitch
                               * (std::size_t) levelRows(data.format, data.height);

        for (auto face = 0; face < faces; ++face)
            uploadFace(static_cast<const std::byte*>(pixels)
                           + (std::size_t) face * faceBytes,
                       pitch,
                       face);

        glBindTexture(data.target, 0);

        return glGetError() == GL_NO_ERROR;
    }

    void update(const void* pixels, int bytesPerRow) override
    {
        if (!data.isValid() || pixels == nullptr)
            return;

        if (bytesPerRow < 0)
            return;

        if (bytesPerRow != 0 && (suppliedChain || data.gl.compressed))
            return;

        context.makeCurrent();

        if (data.mipLevels > 1 || data.cube || data.gl.compressed)
        {
            upload(pixels, bytesPerRow);
            glDrainErrors("texture update");
            return;
        }

        updateRegion(0, 0, data.width, data.height, pixels, bytesPerRow);
    }

    void updateRegion(int x,
                      int y,
                      int regionWidth,
                      int regionHeight,
                      const void* pixels,
                      int bytesPerRow) override
    {
        if (!data.isValid() || pixels == nullptr || bytesPerRow < 0)
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        // A cube has six rectangles this could mean, and a compressed rect
        // would have to land on the 4x4 block grid.
        if (data.cube || data.gl.compressed)
            return;

        if (x < 0 || y < 0 || x + regionWidth > data.width
            || y + regionHeight > data.height)
            return;

        const auto pitch = bytesPerRow != 0
                               ? bytesPerRow
                               : levelBytesPerRow(data.format, regionWidth);

        context.makeCurrent();

        glBindTexture(data.target, data.texture);
        uploadLevel(pixels, pitch, x, y, regionWidth, regionHeight, 0, 0);
        glBindTexture(data.target, 0);

        glDrainErrors("texture region update");
    }

    // The framebuffer a read-back reads out of, and the one a pass will draw
    // into: made the first time one is wanted rather than beside every target.
    GLuint ensureFramebuffer() const
    {
        if (data.framebuffer != 0)
            return data.framebuffer;

        glGenFramebuffers(1, &data.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, data.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               data.texture,
                               0);

        const auto complete =
            glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (complete)
            return data.framebuffer;

        glDeleteFramebuffers(1, &data.framebuffer);
        data.framebuffer = 0;

        return 0;
    }

    // glReadPixels through the target's own framebuffer, on both profiles, and
    // glGetTexImage only where the texture cannot be attached at all - a
    // format this context does not render into, which on desktop still reads
    // back and on ES simply does not.
    void readRegion(int x,
                    int y,
                    int regionWidth,
                    int regionHeight,
                    void* dst,
                    int bytesPerRow) const override
    {
        if (!data.isValid() || dst == nullptr || bytesPerRow < 0)
            return;

        if (regionWidth <= 0 || regionHeight <= 0)
            return;

        if (data.cube || data.gl.compressed)
            return;

        if (x < 0 || y < 0 || x + regionWidth > data.width
            || y + regionHeight > data.height)
            return;

        context.makeCurrent();

        const auto rowBytes = levelBytesPerRow(data.format, regionWidth);
        const auto stride = bytesPerRow != 0 ? bytesPerRow : rowBytes;

        if (ensureFramebuffer() != 0)
            readThroughFramebuffer(x, y, regionWidth, regionHeight, dst, stride);
        else
            readWholeTexture(x, y, regionWidth, regionHeight, dst, stride);

        if (data.gl.swappedBGRA)
            glSwapRedAndBlue(
                static_cast<std::byte*>(dst), regionWidth, regionHeight, stride);

        glDrainErrors("texture read");
    }

    void readThroughFramebuffer(int x,
                                int y,
                                int regionWidth,
                                int regionHeight,
                                void* dst,
                                int stride) const
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, data.framebuffer);

        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        if (bytesPerPixel(data.format) > 0)
            glPixelStorei(GL_PACK_ROW_LENGTH, stride / bytesPerPixel(data.format));

        glReadPixels((GLint) x,
                     (GLint) y,
                     (GLsizei) regionWidth,
                     (GLsizei) regionHeight,
                     data.gl.format,
                     data.gl.type,
                     dst);

        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }

    // glGetTexImage has no region, so the whole level is read into scratch and
    // the rectangle copied out of it.
    void readWholeTexture(int x,
                          int y,
                          int regionWidth,
                          int regionHeight,
                          void* dst,
                          int stride) const
    {
        if (!context.getCapabilities().getTexImage)
            return;

        const auto fullRowBytes = levelBytesPerRow(data.format, data.width);

        scratch.resize((std::size_t) fullRowBytes * (std::size_t) data.height);

        glBindTexture(data.target, data.texture);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glGetTexImage(
            data.target, 0, data.gl.format, data.gl.type, scratch.data());
        glBindTexture(data.target, 0);

        const auto texelBytes = (std::size_t) bytesPerPixel(data.format);
        const auto rowBytes =
            (std::size_t) levelBytesPerRow(data.format, regionWidth);
        auto* out = static_cast<std::byte*>(dst);

        for (auto row = 0; row < regionHeight; ++row)
            std::memcpy(out + (std::size_t) row * (std::size_t) stride,
                        scratch.data()
                            + (std::size_t) (y + row) * (std::size_t) fullRowBytes
                            + (std::size_t) x * texelBytes,
                        rowBytes);
    }

    int width() const override { return data.width; }

    int height() const override { return data.height; }

    bool isValid() const override { return data.isValid(); }

    int mipLevels() const override { return data.mipLevels; }

    bool isRenderTarget() const override { return isValid() && data.renderTarget; }

    bool isCube() const override { return isValid() && data.cube; }

    bool isComputeWritable() const override
    {
        return isValid() && data.computeWrite;
    }

    bool hasDepth() const override { return isValid() && data.depth; }

    bool hasStencil() const override { return isValid() && data.stencil; }

    bool hasSampleableDepth() const override
    {
        return isValid() && data.sampleableDepth;
    }

    int sampleCount() const override { return isValid() ? data.sampleCount : 1; }

    void* nativeTexture() const override { return &data; }

    void* nativeReadView() const override { return &data; }

    // Everything a pass attaches lives in the one struct above, so there is
    // nothing separate to hand back - the D3D12 answer, for the same reason.
    void* nativeDepthTexture() const override { return nullptr; }

    void* nativeMultisampleTexture() const override { return nullptr; }

    void* nativeResolvedDepthTexture() const override { return nullptr; }

    GLContext& context;

    bool suppliedChain = false;

    // glTexStorage2D fixed the levels, so an upload names a sub-image rather
    // than declaring one.
    bool immutable = false;

    // The repacking buffer both the swizzle route and the whole-level read
    // borrow, kept rather than allocated per call.
    mutable std::vector<std::byte> scratch;

    // Mutable because the lazily made framebuffer is wanted inside the const
    // readRegion.
    mutable GLTextureData data;
};
} // namespace

std::unique_ptr<TextureBackend> makeGLTexture(Device& device,
                                              const TextureDescriptor& descriptor,
                                              const void* pixels)
{
    return std::make_unique<GLTextureBackend>(device, descriptor, pixels);
}

// No zero-copy pixel-buffer path on Linux: the texture comes out invalid, as it
// does on D3D12 and on the Vulkan backend beside this one, and Texture::update
// is the path.
std::unique_ptr<TextureBackend> wrapGLPixelBuffer(Device& device,
                                                  void* nativePixelBuffer)
{
    return std::make_unique<GLTextureBackend>(device, nativePixelBuffer);
}
} // namespace eacp::GPU
