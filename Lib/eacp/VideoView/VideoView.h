#pragma once

#include <eacp/Sprites/Sprites.h>
#include <eacp/Video/Player.h>

namespace eacp::Video
{
// How a frame is fitted when its aspect ratio differs from the destination's.
enum class Fit
{
    Stretch, // fill the rect, ignoring aspect (may distort)
    Contain, // fit entirely inside, letterboxing the remainder
    Cover // fill the rect, cropping the overflow
};

// How the frame reaches the GPU. Auto prefers the zero-copy native-buffer path
// (macOS) and falls back to a CPU upload (the only path on Windows for now);
// ZeroCopy and Copy force one path, which is useful for testing.
enum class UploadMode
{
    Auto,
    ZeroCopy,
    Copy
};

// The destination rect for an imageWidth x imageHeight frame inside dst under
// the given fit. Pure geometry, exposed for testing.
Graphics::Rect fitImageArea(const Graphics::Rect& dst,
                            int imageWidth,
                            int imageHeight,
                            Fit fit);

// Draws a Player's current frame through a SpriteRenderer — the composable
// piece of the display path, and what VideoView is built out of. A GPUView
// owns one presenter per video it shows and calls draw() inside its render
// pass, so any number of videos composite with each other and with other GPU
// content (shader passes, sprite overlays) in ONE view on one device — the
// layered-GPU arrangement, without stacking native views.
//
// Frames arrive zero-copy where the backend has a native buffer (macOS) and
// through a reused CPU upload texture otherwise (Windows). Cover crops by
// adjusting the source rect rather than overflowing, so a presenter sharing a
// view with others never bleeds outside dst and over its neighbours.
//
// One presenter per Player: the frame gating is keyed on the player's sequence
// numbers, which are unrelated between players.
class FramePresenter
{
public:
    void setUploadMode(UploadMode mode);

    // Draws the player's latest frame into dst; returns the on-screen rect the
    // image fills (dst for Cover/Stretch), or an empty rect when the player has
    // no frame yet. Reads only what the player's decode thread last published —
    // it never decodes, so it is safe to call from the render path.
    Graphics::Rect draw(Player& player,
                        Sprites::SpriteRenderer& renderer,
                        const Graphics::Rect& dst,
                        Fit fit = Fit::Cover,
                        const Graphics::Color& tint = Graphics::Color::white());

private:
    // Each returns the drawn image area, empty when nothing was drawn.
    Graphics::Rect drawZeroCopy(Player& player,
                                Sprites::SpriteRenderer& renderer,
                                const Graphics::Rect& dst,
                                Fit fit,
                                const Graphics::Color& tint);
    Graphics::Rect drawCpuUpload(Player& player,
                                 Sprites::SpriteRenderer& renderer,
                                 const Graphics::Rect& dst,
                                 Fit fit,
                                 const Graphics::Color& tint);
    Graphics::Rect drawFitted(Sprites::SpriteRenderer& renderer,
                              const GPU::Texture& texture,
                              const Graphics::Rect& dst,
                              Fit fit,
                              const Graphics::Color& tint);

    void resetForPlayer(const Player& player);

    UploadMode uploadMode = UploadMode::Auto;

    // The zero-copy wrap is cached per frame: re-wrapping the same pixel
    // buffer every render costs a texture-cache lookup and an allocation per
    // draw, at display rate, per presenter — for frames that only change at
    // the clip's own frame rate.
    const Player* boundPlayer = nullptr;
    std::optional<GPU::Texture> wrappedTexture;
    std::uint64_t wrappedSequence = 0;

    FramePixels scratch;
    std::optional<GPU::Texture> uploadTexture;
};

// A GPU-backed View that plays one video — the 90% case, and the exact shape
// of Cameras::CameraView. By default each decoded frame schedules a render the
// moment it reaches the main thread (RenderMode::OnFrameArrival), so the view
// redraws at the clip's frame rate rather than the display's: a 24fps clip on
// a 120Hz display renders 24 times a second instead of 120, with the other 96
// no longer re-drawing a frame that never changed.
//
// drawOverlay composites app graphics over the frame in the same pass. The
// view does not own the player; keep it alive while attached (or detach()).
class VideoView : public GPU::GPUView
{
public:
    // What drives redraws while a player is attached.
    enum class RenderMode
    {
        // Render the moment a decoded frame lands (the default): one render
        // per video frame rather than per display refresh. update() and
        // drawOverlay run at the clip's rate, so overlay animation steps with
        // the frames it annotates.
        OnFrameArrival,

        // Redraw every display refresh via the display link: overlays animate
        // at display rate, at the cost of re-rendering unchanged frames.
        Continuous
    };

    VideoView();
    ~VideoView() override;

    void attach(Player& player);
    void detach();

    void setFit(Fit fitToUse);
    void setUploadMode(UploadMode mode);
    void setRenderMode(RenderMode mode);

    // Called after the video image each frame, sharing the same render pass.
    // imageArea is the on-screen rect the video fills; empty when no frame
    // was drawn.
    virtual void drawOverlay(Sprites::SpriteRenderer& renderer,
                             const Graphics::Rect& imageArea);

protected:
    void render(GPU::Frame& frame) override;

private:
    void ensureRenderer();
    void applyRenderMode();

    Player* player = nullptr;

    // Contain, where CameraView defaults to Cover: a camera fills a viewport
    // and the crop goes unnoticed, but cropping a film cuts off the frame the
    // director composed. Letterboxing is the safer default for authored content.
    Fit fit = Fit::Contain;
    RenderMode renderMode = RenderMode::OnFrameArrival;
    FramePresenter presenter;

    // The on-arrival render: update() with display-link-style timing, then an
    // immediate present. State (start/last tick) lives in the callback.
    Callback arrivalTick = [] {};

    // Arrival renders queued onto the main thread check this before touching
    // the view, so one queued behind a teardown backs off instead of dangling.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::optional<Sprites::SpriteRenderer> renderer;
};
} // namespace eacp::Video
