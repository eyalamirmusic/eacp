#pragma once

#include <eacp/Camera/Camera.h>
#include <eacp/Sprites/Sprites.h>

namespace eacp::Cameras
{
// A GPU-backed View that renders a camera's live feed. By default each
// captured frame schedules a render the moment it reaches the main thread
// (RenderMode::OnFrameArrival), so it goes to glass at the next refresh
// instead of waiting to be noticed by a display link tick. The frame is
// wrapped zero-copy as a texture (macOS) and drawn through the sprite
// renderer, then drawOverlay composites AI results (landmarks, boxes) in the
// same GPU pass. The view does not own the camera; keep the camera alive
// while it is attached (or detach() it).
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

    // How the frame reaches the GPU. Auto prefers the zero-copy native-buffer
    // path (macOS) and falls back to a CPU upload (the only path on Windows for
    // now); ZeroCopy and Copy force one path, which is useful for testing.
    enum class UploadMode
    {
        Auto,
        ZeroCopy,
        Copy
    };

    // What drives redraws while a camera is attached.
    enum class RenderMode
    {
        // Render the moment a captured frame lands (the default): the lowest
        // glass latency, and one render per camera frame rather than per
        // display refresh. update() and drawOverlay run at the camera's rate,
        // so overlay animation steps with the frames it annotates.
        OnFrameArrival,

        // Redraw every display refresh via the display link: overlays animate
        // at display rate, and each frame waits for the next tick (up to one
        // refresh) before it is picked up.
        Continuous
    };

    void attach(Camera& camera);
    void detach();

    void setFit(Fit fitToUse);
    void setMirrored(bool mirroredToUse); // horizontal flip, e.g. front cameras
    void setUploadMode(UploadMode mode);
    void setRenderMode(RenderMode mode);

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
    void applyRenderMode();
    Graphics::Rect imageAreaFor(int textureWidth, int textureHeight) const;

    // Each returns whether a camera image was drawn and, if so, sets imageArea.
    bool renderZeroCopy(Graphics::Rect& imageArea);
    bool renderCpuUpload(Graphics::Rect& imageArea);

    Camera* camera = nullptr;
    Fit fit = Fit::Cover;
    bool mirrored = false;
    UploadMode uploadMode = UploadMode::Auto;
    RenderMode renderMode = RenderMode::OnFrameArrival;

    // The on-arrival render: update() with display-link-style timing, then an
    // immediate present. State (start/last tick) lives in the callback.
    Callback arrivalTick = [] {};

    // Arrival renders queued onto the main thread check this before touching
    // the view, so one queued behind a teardown backs off instead of dangling.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::optional<Sprites::SpriteRenderer> renderer;
    Graphics::Point rendererSize {0.0f, 0.0f};

    // CPU-upload path: a frame reused across calls and the texture it feeds.
    FramePixels scratch;
    std::optional<GPU::Texture> uploadTexture;
};
} // namespace eacp::Cameras
