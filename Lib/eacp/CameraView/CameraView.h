#pragma once

#include <eacp/Camera/Camera.h>
#include <eacp/Sprites/Sprites.h>

namespace eacp::Cameras
{
// Renders a camera's live feed. Does not own the camera: keep it alive while
// attached, or detach() first.
class CameraView : public GPU::GPUView
{
public:
    CameraView();
    ~CameraView() override;

    using Fit = Sprites::Fit;

    // Auto prefers the zero-copy native-buffer path (macOS) and falls back to a
    // CPU upload; the others force one path, for testing.
    enum class UploadMode
    {
        Auto,
        ZeroCopy,
        Copy
    };

    // What drives redraws while a camera is attached: once per captured frame,
    // or once per display refresh via the display link.
    enum class RenderMode
    {
        OnFrameArrival,
        Continuous
    };

    void attach(Camera& camera);
    void detach();

    void setFit(Fit fitToUse);
    void setMirrored(bool mirroredToUse); // horizontal flip
    void setUploadMode(UploadMode mode);
    void setRenderMode(RenderMode mode);

    // Shares the camera image's render pass. imageArea is the on-screen rect in
    // logical points it fills, empty when no frame was drawn.
    virtual void drawOverlay(Sprites::SpriteRenderer& renderer,
                             const Graphics::Rect& imageArea);

    static Graphics::Rect computeImageArea(float viewWidth,
                                           float viewHeight,
                                           int textureWidth,
                                           int textureHeight,
                                           Fit fit)
    {
        return Sprites::fitRect(
            viewWidth, viewHeight, textureWidth, textureHeight, fit);
    }

protected:
    void render(GPU::Frame& frame) override;

private:
    void ensureRenderer();
    void applyRenderMode();
    Graphics::Rect imageAreaFor(int textureWidth, int textureHeight) const;

    // Each returns whether an image was drawn and, if so, sets imageArea.
    bool renderZeroCopy(Graphics::Rect& imageArea);
    bool renderCpuUpload(Graphics::Rect& imageArea);

    Camera* camera = nullptr;
    Fit fit = Fit::Cover;
    bool mirrored = false;
    UploadMode uploadMode = UploadMode::Auto;
    RenderMode renderMode = RenderMode::OnFrameArrival;

    Callback arrivalTick = [] {};

    // Renders queued onto the main thread check this, so one queued behind a
    // teardown backs off instead of dangling.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::optional<Sprites::SpriteRenderer> renderer;
    Graphics::Point rendererSize {0.0f, 0.0f};

    FramePixels scratch;
    std::optional<GPU::Texture> uploadTexture;
};
} // namespace eacp::Cameras
