#pragma once

#include <eacp/Sprites/Sprites.h>
#include <eacp/Video/Decode/Player.h>

namespace eacp::Video
{
// Placement and colour appear throughout this module, so they are hoisted here
// rather than qualified at each mention. Scoped to the module deliberately:
// pulling them into all of eacp would collide with any user type of the same
// name reached through `using namespace eacp`.
using Graphics::Color;
using Graphics::Point;
using Graphics::Rect;

// A GPU-backed View that draws a video stream. The frame for the current time
// is wrapped zero-copy as a texture (Apple) or uploaded (Windows) and drawn
// through the sprite renderer, then drawOverlay composites anything else — a
// scrub bar, subtitles, an editor's guides — in the same GPU pass.
//
// Two ways to drive it, and they are the two halves of the design:
//
//   attach(Player&)      the view runs the clock from the display link, which
//                        is what a media player wants.
//
//   attach(FrameStream&) the view draws whatever setTime() asks for, and the
//                        clock stays with the caller — a game's simulation
//                        time, an editor's playhead.
//
// The view owns neither; keep them alive while it is attached, or detach().
class VideoView : public GPU::GPUView
{
public:
    VideoView();
    ~VideoView() override;

    // How the frame is fitted when its aspect ratio differs from the view's.
    using Fit = Sprites::Fit;

    // How the frame reaches the GPU. Auto prefers the zero-copy native-buffer
    // path and falls back to a CPU upload for backends that decode into main
    // memory; ZeroCopy and Copy force one path, which is useful for testing.
    enum class UploadMode
    {
        Auto,
        ZeroCopy,
        Copy
    };

    // Plays `player`, advancing its clock once per display refresh. The view
    // switches to continuous rendering, since the picture changes on its own.
    void attach(Player& player);

    // Draws `stream` at whatever time setTime() was last given. Rendering stays
    // on-demand: call setTime() (which repaints) from your own loop.
    void attach(FrameStream& stream);

    void detach();

    // The time to draw, for a caller-driven stream. Ignored while a Player is
    // attached, which owns the playhead itself.
    void setTime(double seconds);
    double time() const;

    void setFit(Fit fitToUse);
    void setMirrored(bool mirroredToUse);
    void setUploadMode(UploadMode mode);

    // Called after the video image each frame, sharing the same render pass.
    // imageArea is the on-screen rect (logical points) the video fills, for
    // aligning overlays; it is empty (zero size) when no frame was drawn.
    //
    // The pass comes through as well as the sprite renderer so an overlay can
    // bring its own: text, in particular, goes through Text::TextRenderer
    // rather than through sprites, and needs somewhere to flush to.
    virtual void drawOverlay(GPU::RenderPass& pass,
                             Sprites::SpriteRenderer& renderer,
                             const Rect& imageArea);

    // Whether the frame just rendered reached the GPU without a copy. False
    // when it went through the CPU upload path, or when nothing was drawn.
    bool lastFrameWasZeroCopy() const { return zeroCopyLastFrame; }

    // Where a texture's top-left corner lands and where its +u and +v axes go,
    // to place a textureWidth x textureHeight frame into `area` under a display
    // rotation and an optional horizontal mirror.
    //
    // A rotation is not a flip: at 90 degrees the texture's u axis runs *down*
    // the screen, which flipX/flipY cannot express, so this produces the
    // parallelogram SpriteRenderer::drawTextureQuad takes. Pure geometry,
    // exposed for testing.
    struct Placement
    {
        Point origin;
        Point edgeX;
        Point edgeY;
    };

    static Placement
        computePlacement(const Rect& area, int rotationDegrees, bool mirrored);

    // The size a frame is displayed at, which swaps width and height for a
    // quarter-turn rotation. This is what the fit is computed against — a
    // portrait phone clip is stored landscape and must be letterboxed as the
    // portrait it will be shown as.
    static Point
        displaySize(int textureWidth, int textureHeight, int rotationDegrees);

protected:
    void render(GPU::Frame& frame) override;
    void update(Threads::FrameTime frameTime) override;

private:
    void ensureRenderer();
    void applyFrameReadyCallback();
    Rect imageAreaFor(int textureWidth, int textureHeight) const;

    // Draws one texture into the view under the current fit, rotation and
    // mirror, and reports the on-screen rect it filled.
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

    // The CPU-upload path's textures, reused across frames and rebuilt only
    // when the frame size or pixel format changes. A BGRA frame uses only the
    // first; an NV12 frame uses both, the second carrying the half-size chroma
    // plane.
    std::optional<GPU::Texture> uploadTexture;
    std::optional<GPU::Texture> chromaTexture;
    FramePixelFormat uploadedFormat = FramePixelFormat::BGRA8;

    // The frame the last upload came from, so a repaint that draws the same
    // frame again does not re-upload it.
    const void* uploadedPixels = nullptr;

    // Repaints queued from the decode thread check this before touching the
    // view, so one arriving behind a teardown backs off instead of dangling.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
} // namespace eacp::Video
