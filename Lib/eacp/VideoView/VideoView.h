#pragma once

#include <eacp/Sprites/Sprites.h>
#include <eacp/Video/Decode/Player.h>

namespace eacp::Video
{
using Graphics::Color;
using Graphics::Point;
using Graphics::Rect;

// Draws a video stream, either from a Player whose clock it runs off the
// display link, or from a FrameStream at whatever time setTime() was given. It
// owns neither: keep them alive while attached, or detach() first.
class VideoView : public GPU::GPUView
{
public:
    VideoView();
    ~VideoView() override;

    using Fit = Sprites::Fit;

    // Auto prefers the zero-copy native-buffer path and falls back to a CPU
    // upload; the others force one path, for testing.
    enum class UploadMode
    {
        Auto,
        ZeroCopy,
        Copy
    };

    // Advances the player's clock once per display refresh, switching the view
    // to continuous rendering.
    void attach(Player& player);

    // Leaves rendering on-demand: setTime() repaints.
    void attach(FrameStream& stream);

    void detach();

    // Ignored while a Player is attached, which owns the playhead itself.
    void setTime(double seconds);
    double time() const;

    void setFit(Fit fitToUse);
    void setMirrored(bool mirroredToUse);
    void setUploadMode(UploadMode mode);

    // Shares the video image's render pass. imageArea is the on-screen rect in
    // logical points it fills, empty when no frame was drawn. The pass comes
    // through too, since text renders outside the sprite renderer.
    virtual void drawOverlay(GPU::RenderPass& pass,
                             Sprites::SpriteRenderer& renderer,
                             const Rect& imageArea);

    // False when the last frame went through the CPU upload path, and when
    // nothing was drawn.
    bool lastFrameWasZeroCopy() const { return zeroCopyLastFrame; }

    // Where a texture's top-left corner lands and where its +u and +v axes go.
    // A rotation is not a flip — at 90 degrees the u axis runs *down* the
    // screen — hence the parallelogram drawTextureQuad takes.
    struct Placement
    {
        Point origin;
        Point edgeX;
        Point edgeY;
    };

    static Placement
        computePlacement(const Rect& area, int rotationDegrees, bool mirrored);

    // Swaps width and height for a quarter-turn rotation. What the fit is
    // computed against, so a portrait clip stored landscape letterboxes right.
    static Point
        displaySize(int textureWidth, int textureHeight, int rotationDegrees);

protected:
    void render(GPU::Frame& frame) override;
    void update(Threads::FrameTime frameTime) override;

private:
    void ensureRenderer();
    void applyFrameReadyCallback();
    Rect imageAreaFor(int textureWidth, int textureHeight) const;

    // Reports the on-screen rect the texture filled.
    void drawFrameTexture(const GPU::Texture& texture, Rect& imageArea);

    // Each returns whether the frame was drawn and, if so, sets imageArea.
    bool drawZeroCopy(const VideoFrame& frame, Rect& imageArea);
    bool drawUpload(const VideoFrame& frame, Rect& imageArea);

    Player* player = nullptr;
    FrameStream* stream = nullptr;

    double playhead = 0.0;
    Fit fit = Fit::Contain;
    bool mirrored = false;

    // The attached track's display rotation, refreshed each render.
    int rotation = 0;

    bool zeroCopyLastFrame = false;
    UploadMode uploadMode = UploadMode::Auto;

    std::optional<Sprites::SpriteRenderer> renderer;
    Point rendererSize {0.0f, 0.0f};

    // Rebuilt only when the frame size or pixel format changes. A BGRA frame
    // uses only the first; NV12 puts its half-size chroma plane in the second.
    std::optional<GPU::Texture> uploadTexture;
    std::optional<GPU::Texture> chromaTexture;
    FramePixelFormat uploadedFormat = FramePixelFormat::BGRA8;

    // The frame the last upload came from, so redrawing it does not re-upload.
    const void* uploadedPixels = nullptr;

    // Repaints queued from the decode thread check this, so one arriving behind
    // a teardown backs off instead of dangling.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
} // namespace eacp::Video
