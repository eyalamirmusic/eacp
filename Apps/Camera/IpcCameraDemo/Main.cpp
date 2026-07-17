#include <eacp/CameraView/CameraView.h>
#include <eacp/GPU/GPU.h>
#include <eacp/Network/Network.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>

// One camera, two processes. The first instance claims the channel name,
// opens the camera and shows it locally through CameraView's zero-copy
// path, then launches this same executable again; the second instance
// loses the claim and dials in as a viewer. Every fresh frame is shipped
// over the message channel as [u32 width][u32 height][BGRA bytes] and
// drawn on the viewer's GPU view with the same CPU-upload path Windows
// display uses. The pair exercises the whole pipeline: capture, zero-copy
// local display, IPC transport, remote texture upload.
//
// Frames ride the raw Messenger, not the RPC bridge: megabytes of pixels
// are exactly what the length-prefixed binary framing is for, and exactly
// what JSON is not.
using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto channelName = "com.eacp.ipccamdemo";
constexpr auto frameHeaderBytes = std::size_t {8};

void appendU32(std::string& out, std::uint32_t value)
{
    for (auto shift = 0; shift < 32; shift += 8)
        out += (char) ((value >> shift) & 0xff);
}

std::uint32_t readU32(const char* bytes)
{
    auto value = std::uint32_t {0};

    for (auto index = std::size_t {0}; index < 4; ++index)
        value |= (std::uint32_t) (unsigned char) bytes[index] << (index * 8);

    return value;
}

std::string encodeFrame(const Cameras::CameraFrame& frame)
{
    auto width = (std::size_t) frame.width();
    auto height = (std::size_t) frame.height();
    auto rowBytes = width * 4;

    auto message = std::string {};
    message.reserve(frameHeaderBytes + rowBytes * height);

    appendU32(message, (std::uint32_t) frame.width());
    appendU32(message, (std::uint32_t) frame.height());

    const auto* pixels = (const char*) frame.data();

    if (frame.bytesPerRow() == rowBytes)
        message.append(pixels, rowBytes * height);
    else
        for (auto row = std::size_t {0}; row < height; ++row)
            message.append(pixels + row * frame.bytesPerRow(), rowBytes);

    return message;
}

WindowOptions windowOptionsFor(bool isCamera)
{
    auto options = WindowOptions {};
    options.width = 960;
    options.height = 540;
    options.title = isCamera ? "IPC Camera - capturing" : "IPC Camera - Viewer";
    options.initialPosition = isCamera ? Point {80.f, 120.f} : Point {1060.f, 120.f};
    return options;
}
} // namespace

// The receiving end of the feed: whole BGRA frames arrive on the main
// thread and the next vsync uploads them into a persistent texture - the
// render body is CameraView::renderCpuUpload with the camera swapped for
// a remote peer.
struct RemoteFrameView final : GPU::GPUView
{
    RemoteFrameView()
    {
        setSampleCount(1);
        setContinuous(true);
    }

    void showFrame(int frameWidth, int frameHeight, std::string pixelsToUse)
    {
        width = frameWidth;
        height = frameHeight;
        pixels = std::move(pixelsToUse);
        fresh = true;

        if (++framesShown % 30 == 0)
            std::printf(
                "viewer showed %d frames (%dx%d)\n", framesShown, width, height);
    }

    void render(GPU::Frame& frame) override
    {
        auto bounds = getLocalBounds();
        auto size = Point {bounds.w, bounds.h};

        if (!renderer.has_value() || size.x != rendererSize.x
            || size.y != rendererSize.y)
        {
            renderer.emplace(size, sampleCount());
            rendererSize = size;
        }

        auto pass = frame.beginPass({Color::black()});
        renderer->begin(pass);

        if (fresh)
        {
            auto sizeChanged = !texture.has_value() || texture->width() != width
                               || texture->height() != height;

            if (sizeChanged)
            {
                auto descriptor = GPU::TextureDescriptor {};
                descriptor.width = width;
                descriptor.height = height;
                descriptor.format = GPU::TextureFormat::BGRA8Unorm;
                texture.emplace(GPU::Device::shared().makeTexture(descriptor));
            }

            texture->update((const std::uint8_t*) pixels.data());
            fresh = false;
        }

        if (!texture.has_value() || !texture->isValid() || width <= 0)
            return;

        auto imageArea = Cameras::CameraView::computeImageArea(
            bounds.w, bounds.h, width, height, Cameras::CameraView::Fit::Contain);
        renderer->drawTexture(*texture, imageArea, false, false);
    }

    int width = 0;
    int height = 0;
    std::string pixels;
    bool fresh = false;
    int framesShown = 0;

    std::optional<Sprites::SpriteRenderer> renderer;
    Point rendererSize {0.f, 0.f};
    std::optional<GPU::Texture> texture;
};

struct IpcCameraApp
{
    // Claiming the name is the role decision: winner captures, loser views.
    IpcCameraApp()
    {
        try
        {
            server.emplace(channelName);
        }
        catch (const IPC::Error&)
        {
        }

        if (server)
            becomeCamera();
        else
            becomeViewer();

        armAutoQuit();
    }

    ~IpcCameraApp()
    {
        if (camera)
            camera->stop();
    }

    void becomeCamera()
    {
        server->onClient = [this](IPC::Messenger& viewer)
        {
            {
                auto guard = std::lock_guard {viewersMutex};
                viewers.add(&viewer);
            }

            updateCameraTitle();

            viewer.onDisconnected = [this, leaving = &viewer]
            {
                {
                    auto guard = std::lock_guard {viewersMutex};
                    viewers.eraseIf([leaving](IPC::Messenger* candidate)
                                    { return candidate == leaving; });
                }

                updateCameraTitle();
            };
        };

        launchSecondInstance();

        camera.emplace();
        camera->setFrameCallback([this](const Cameras::CameraFrame& frame)
                                 { ship(frame); });
        cameraView.emplace();
        cameraView->setMirrored(true);
        cameraView->attach(*camera);

        window.emplace(windowOptionsFor(true));
        window->setContentView(*cameraView);

        beginCapture();
    }

    void becomeViewer()
    {
        remoteView.emplace();
        window.emplace(windowOptionsFor(false));
        window->setContentView(*remoteView);

        link.emplace(channelName);
        link->onConnected = [this]
        { window->setTitle("IPC Camera - Viewer (live)"); };
        link->onDisconnected = [this]
        { window->setTitle("IPC Camera - Viewer (feed ended)"); };
        link->onMessage = [this](const std::string& message) { showFrame(message); };
    }

    // Runs on the capture thread the moment a frame lands, so nothing
    // waits on a poll tick. Messenger::send is safe from any thread; the
    // mutex only fences the viewer list against arrivals and departures
    // on the main thread. A viewer that stops draining stalls this thread,
    // and the camera's discardLateFrames turns the stall into dropped
    // frames - latest wins.
    void ship(const Cameras::CameraFrame& frame)
    {
        if (frame.data() == nullptr || frame.width() <= 0 || frame.height() <= 0)
            return;

        auto guard = std::lock_guard {viewersMutex};

        if (viewers.size() == 0)
            return;

        auto message = encodeFrame(frame);

        for (auto* viewer: viewers)
            viewer->send(message);

        if (++framesSent % 30 == 0)
            std::printf("camera shipped %d frames (%dx%d)\n",
                        framesSent,
                        frame.width(),
                        frame.height());
    }

    void showFrame(const std::string& message)
    {
        if (message.size() < frameHeaderBytes)
            return;

        auto width = (int) readU32(message.data());
        auto height = (int) readU32(message.data() + 4);
        auto expected = (std::size_t) width * (std::size_t) height * 4;

        if (width <= 0 || height <= 0
            || message.size() - frameHeaderBytes != expected)
            return;

        remoteView->showFrame(width, height, message.substr(frameHeaderBytes));
    }

    void beginCapture()
    {
        switch (Cameras::Camera::permissionStatus())
        {
            case Cameras::PermissionStatus::Granted:
                startCamera();
                break;
            case Cameras::PermissionStatus::NotDetermined:
                // Headless runs (CI) must not summon the system permission
                // dialog; they simply show no image.
                if (!Apps::getAppEnvironment().headless)
                    Cameras::Camera::requestPermission(
                        [this](bool granted)
                        {
                            if (granted)
                                startCamera();
                        });
                break;
            default:
                std::printf("Camera access not granted; nothing to ship.\n");
                break;
        }
    }

    void startCamera()
    {
        auto config = Cameras::CameraConfig {};
        config.width = 1280;
        config.height = 720;
        camera->start(config);
    }

    void updateCameraTitle()
    {
        auto count = viewers.size();
        window->setTitle("IPC Camera - capturing (" + std::to_string(count)
                         + (count == 1 ? " viewer)" : " viewers)"));
    }

    void launchSecondInstance()
    {
        auto& arguments = Apps::getAppEnvironment().commandLineArgs;

        if (arguments.size() == 0)
            return;

        auto options = Processes::ProcessOptions {};
        options.executable = arguments[0];
        options.captureOutput = false;
        child.emplace(std::move(options));
    }

    void armAutoQuit()
    {
        auto seconds = std::atof(getEnvValue("EACP_DEMO_AUTOQUIT_SECONDS").c_str());

        if (seconds <= 0.0)
            return;

        quitDeadline.emplace(Time::MS {(std::int64_t) (seconds * 1000.0)});
        quitTimer.emplace(
            [this]
            {
                if (quitDeadline->expired())
                    Apps::quit();
            },
            4);
    }

    std::optional<IPC::MessageServer> server;
    std::mutex viewersMutex;
    Vector<IPC::Messenger*> viewers;
    std::optional<Cameras::Camera> camera;
    std::optional<Cameras::CameraView> cameraView;
    int framesSent = 0;

    std::optional<IPC::Messenger> link;
    std::optional<RemoteFrameView> remoteView;

    std::optional<Window> window;
    std::optional<Processes::Process> child;
    std::optional<Threads::Timer> quitTimer;
    std::optional<Time::Deadline> quitDeadline;
};

int main(int argc, char* argv[])
{
    return Apps::run<IpcCameraApp>(argc, argv);
}
