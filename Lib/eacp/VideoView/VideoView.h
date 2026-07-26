#pragma once

#include <eacp/Sprites/Sprites.h>
#include <eacp/Video/Decode/Player.h>

namespace eacp::Video
{
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
    virtual void drawOverlay(Sprites::SpriteRenderer& renderer,
                             const Graphics::Rect& imageArea);

protected:
    void render(GPU::Frame& frame) override;
    void update(Threads::FrameTime frameTime) override;

private:
    void ensureRenderer();
    Graphics::Rect imageAreaFor(int textureWidth, int textureHeight) const;

    // Each returns whether the frame was drawn and, if so, sets imageArea.
    bool drawZeroCopy(const VideoFrame& frame, Graphics::Rect& imageArea);
    bool drawUpload(const VideoFrame& frame, Graphics::Rect& imageArea);

    Player* player = nullptr;
    FrameStream* stream = nullptr;

    double playhead = 0.0;
    Fit fit = Fit::Contain;
    bool mirrored = false;
    UploadMode uploadMode = UploadMode::Auto;

    std::optional<Sprites::SpriteRenderer> renderer;
    Graphics::Point rendererSize {0.0f, 0.0f};

    // The CPU-upload path's texture, reused across frames and rebuilt only when
    // the frame size changes.
    std::optional<GPU::Texture> uploadTexture;

    // The frame the last upload came from, so a repaint that draws the same
    // frame again does not re-upload it.
    const void* uploadedPixels = nullptr;
};
} // namespace eacp::Video
