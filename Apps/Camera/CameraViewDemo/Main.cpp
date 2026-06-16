#include <eacp/Camera/Camera.h>
#include <eacp/CameraView/CameraView.h>
#include <eacp/Core/App/App.h>
#include <eacp/Graphics/Graphics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace eacp;

namespace
{
Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1280;
    options.height = 720;
    options.title = "eacp Camera";
    options.flags = {Graphics::WindowFlags::Titled,
                     Graphics::WindowFlags::Closable,
                     Graphics::WindowFlags::Miniaturizable,
                     Graphics::WindowFlags::Resizable};
    return options;
}

// The camera view plus an animated overlay drawn in the same GPU pass — standing
// in for AI results (landmarks, boxes). It also self-reports render progress so
// the display path can be checked without eyes on the window.
struct DemoCameraView final : Cameras::CameraView
{
    void update(Threads::FrameTime frameTime) override { elapsed = frameTime.time; }

    void drawOverlay(Sprites::SpriteRenderer& renderer,
                     const Graphics::Rect& imageArea) override
    {
        ++overlayTicks;

        if (imageArea.w > 0.0f && imageArea.h > 0.0f)
            ++framesWithImage;

        auto bounds = getLocalBounds();
        auto center = bounds.center();
        auto radius = 0.25f * std::min(bounds.w, bounds.h);

        // A box orbiting the centre: animated, proving the overlay composites
        // over the live camera texture in one pass.
        auto bx = center.x + std::cos((float) elapsed) * radius;
        auto by = center.y + std::sin((float) elapsed) * radius;
        renderer.fillRect({bx - 12.0f, by - 12.0f, 24.0f, 24.0f},
                          {1.0f, 0.85f, 0.2f, 0.85f});

        renderer.drawLine({center.x - 24.0f, center.y},
                          {center.x + 24.0f, center.y},
                          {1.0f, 1.0f, 1.0f, 0.9f},
                          2.0f);
        renderer.drawLine({center.x, center.y - 24.0f},
                          {center.x, center.y + 24.0f},
                          {1.0f, 1.0f, 1.0f, 0.9f},
                          2.0f);
        renderer.drawRect(bounds, {0.2f, 1.0f, 0.45f, 0.8f}, 3.0f);

        if (overlayTicks % 30 == 0)
            std::printf("render tick %d  (frames with camera image: %d)\n",
                        overlayTicks,
                        framesWithImage);

        if (autoQuitSeconds > 0.0 && elapsed >= autoQuitSeconds)
        {
            std::printf("auto-quit after %.1fs: %d ticks, %d with image\n",
                        elapsed,
                        overlayTicks,
                        framesWithImage);
            Apps::quit();
        }
    }

    double elapsed = 0.0;
    int overlayTicks = 0;
    int framesWithImage = 0;
    double autoQuitSeconds = 0.0;
};

struct CameraApp
{
    CameraApp()
    {
        if (const auto* env = std::getenv("EACP_DEMO_AUTOQUIT_SECONDS"))
            view.autoQuitSeconds = std::atof(env);

        // Force the CPU-upload display path (Windows uses it) for verification.
        if (const auto* mode = std::getenv("EACP_DEMO_UPLOAD_MODE"))
            if (std::strcmp(mode, "copy") == 0)
                view.setUploadMode(Cameras::CameraView::UploadMode::Copy);

        view.setMirrored(true); // front-camera-style preview
        view.attach(camera);
        window.setContentView(view);
        beginCapture();
    }

    ~CameraApp() { camera.stop(); }

    void startCamera()
    {
        auto config = Cameras::CameraConfig {};
        config.width = 1280;
        config.height = 720;
        camera.start(config);
    }

    void beginCapture()
    {
        switch (Cameras::Camera::permissionStatus())
        {
            case Cameras::PermissionStatus::Granted:
                startCamera();
                break;
            case Cameras::PermissionStatus::NotDetermined:
                Cameras::Camera::requestPermission(
                    [this](bool granted)
                    {
                        if (granted)
                            startCamera();
                    });
                break;
            default:
                std::printf("Camera access not granted; showing overlay only.\n");
                break;
        }
    }

    Cameras::Camera camera;
    DemoCameraView view;
    Graphics::Window window {makeOptions()};
};
} // namespace

int main()
{
    eacp::Apps::run<CameraApp>();
    return 0;
}
