#pragma once

#include <eacp/Camera/Camera.h>
#include <eacp/GPU/View/GPUView.h>
#include <eacp/Graphics/Primitives/Primitives.h>
#include <eacp/Sprites/SpriteRenderer.h>

#include <optional>

namespace eacp::Cameras
{
// A GPU-backed View that renders a camera's live feed. Each display refresh it
// pulls the camera's latest frame, wraps it zero-copy as a texture (macOS) and
// draws it through the sprite renderer, then calls drawOverlay so AI results
// (landmarks, boxes) can be composited in the same GPU pass. The view does not
// own the camera; keep the camera alive while it is attached (or detach() it).
class CameraView : public GPU::GPUView
{
public:
    CameraView();
    ~CameraView() override;

    // How the frame is fitted when its aspect ratio differs from the view's.
    enum class Fit
    {
        Stretch, // fill the view, ignoring aspect (may distort)
        Contain, // fit entirely inside, letterboxing the remainder
        Cover // fill the view, cropping the overflow (default)
    };

    void attach(Camera& camera);
    void detach();

    void setFit(Fit fitToUse);
    void setMirrored(bool mirroredToUse); // horizontal flip, e.g. front cameras

    // Called after the camera image each frame, sharing the same render pass.
    // imageArea is the on-screen rect (logical points) the camera image fills,
    // for aligning overlays; it is empty (zero size) when no frame was drawn.
    virtual void drawOverlay(Sprites::SpriteRenderer& renderer,
                             const Graphics::Rect& imageArea);

    // The destination rect for a texWidth x texHeight image inside a
    // viewWidth x viewHeight view under the given fit. Pure geometry, exposed
    // for testing.
    static Graphics::Rect computeImageArea(float viewWidth,
                                           float viewHeight,
                                           int textureWidth,
                                           int textureHeight,
                                           Fit fit);

protected:
    void render(GPU::Frame& frame) override;

private:
    void ensureRenderer();

    Camera* camera = nullptr;
    Fit fit = Fit::Cover;
    bool mirrored = false;

    std::optional<Sprites::SpriteRenderer> renderer;
    Graphics::Point rendererSize {0.0f, 0.0f};
};
} // namespace eacp::Cameras
