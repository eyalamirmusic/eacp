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
    Cover // fill the rect, cropping the overflow (default)
};

// The destination rect for an imageWidth x imageHeight frame inside dst under
// the given fit. Pure geometry, exposed for testing.
Graphics::Rect fitImageArea(const Graphics::Rect& dst,
                            int imageWidth,
                            int imageHeight,
                            Fit fit);

// Draws a Player's current frame through a SpriteRenderer — the composable
// piece of the display path. A GPUView owns one presenter per video it shows
// and calls draw() inside its render pass, so any number of videos composite
// with each other and with other GPU content (shader passes, sprite overlays)
// in ONE view on one device — the layered-GPU arrangement, without stacking
// native views.
//
// Frames arrive zero-copy where the backend has a native buffer (macOS) and
// through a reused CPU upload texture otherwise (Windows). Cover/Contain
// crops or letterboxes; the presenter clips Cover overflow by adjusting the
// source rect, so nothing bleeds outside dst.
class FramePresenter
{
public:
    // Draws the frame due now into dst; returns the on-screen rect the image
    // fills (dst for Cover/Stretch), or an empty rect when the player has no
    // frame yet.
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

    // The zero-copy wrap is cached per frame: re-wrapping the same pixel
    // buffer every render costs a texture-cache lookup and an allocation per
    // draw, at display rate, per presenter — for frames that only change at
    // the clip's own frame rate.
    const Player* boundPlayer = nullptr;
    std::optional<GPU::Texture> wrappedTexture;
    std::uint64_t wrappedSequence = 0;

    PlayerFramePixels scratch;
    std::optional<GPU::Texture> uploadTexture;
};

// A GPU-backed View that plays one video — the 90% case. The view renders
// continuously while a player is attached (video is continuous content);
// drawOverlay composites app graphics over the frame in the same pass. The
// view does not own the player; keep it alive while attached (or detach()).
class VideoView : public GPU::GPUView
{
public:
    VideoView();

    void attach(Player& player);
    void detach();

    void setFit(Fit fitToUse);

    // Called after the video image each frame, sharing the same render pass.
    // imageArea is the on-screen rect the video fills; empty when no frame
    // was drawn.
    virtual void drawOverlay(Sprites::SpriteRenderer& renderer,
                             const Graphics::Rect& imageArea);

protected:
    void render(GPU::Frame& frame) override;

private:
    void ensureRenderer();

    Player* player = nullptr;
    Fit fit = Fit::Contain;
    FramePresenter presenter;

    std::optional<Sprites::SpriteRenderer> renderer;
    Graphics::Point rendererSize {0.0f, 0.0f};
};
} // namespace eacp::Video
