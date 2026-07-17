#include <eacp/CameraView/CameraView.h>
#include <algorithm>

#include <cstdio>
#include <cstdlib>
#include <optional>

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
    }

    double elapsed = 0.0;
    int overlayTicks = 0;
    int framesWithImage = 0;
};

struct CameraApp
{
    CameraApp()
    {
        // Force the CPU-upload display path (Windows uses it) for verification.
        if (getEnvValue("EACP_DEMO_UPLOAD_MODE") == "copy")
            view.setUploadMode(Cameras::CameraView::UploadMode::Copy);

        view.setMirrored(true); // front-camera-style preview
        view.attach(camera);
        window.setContentView(view);
        beginCapture();
        armAutoQuit();
    }

    ~CameraApp() { camera.stop(); }

    void startCamera()
    {
        auto config = Cameras::CameraConfig {};
        config.width = 1280;
        config.height = 720;
        camera.start(config);
    }

    // Without frames the default on-arrival mode never renders; fall back to
    // the display link so the overlay still animates.
    void showOverlayOnly()
    {
        std::printf("Camera access not granted; showing overlay only.\n");
        view.setRenderMode(Cameras::CameraView::RenderMode::Continuous);
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
                        else
                            showOverlayOnly();
                    });
                break;
            default:
                showOverlayOnly();
                break;
        }
    }

    // Timer-driven, not render-driven, so it fires even when no camera ever
    // delivers a frame.
    void armAutoQuit()
    {
        auto seconds = std::atof(getEnvValue("EACP_DEMO_AUTOQUIT_SECONDS").c_str());

        if (seconds <= 0.0)
            return;

        quitDeadline.emplace(Time::MS {(std::int64_t) (seconds * 1000.0)});
        quitTimer.emplace(
            [this]
            {
                if (!quitDeadline->expired())
                    return;

                std::printf("auto-quit: %d ticks, %d with image\n",
                            view.overlayTicks,
                            view.framesWithImage);
                Apps::quit();
            },
            100);
    }

    Cameras::Camera camera;
    DemoCameraView view;
    Graphics::Window window {makeOptions()};
    std::optional<Threads::Timer> quitTimer;
    std::optional<Time::Deadline> quitDeadline;
};
} // namespace

int main()
{
    return eacp::Apps::run<CameraApp>();
}
