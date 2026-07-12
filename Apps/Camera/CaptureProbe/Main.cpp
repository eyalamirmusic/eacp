#include <eacp/Camera/Camera.h>
#include <eacp/Graphics/Graphics.h>

#include <atomic>
#include <cstdio>

using namespace eacp;

namespace
{
const char* toString(Cameras::PermissionStatus status)
{
    switch (status)
    {
        case Cameras::PermissionStatus::Granted:
            return "Granted";
        case Cameras::PermissionStatus::Denied:
            return "Denied";
        case Cameras::PermissionStatus::NotDetermined:
            return "NotDetermined";
        case Cameras::PermissionStatus::Restricted:
            return "Restricted";
    }

    return "Unknown";
}

// A cheap content hash over the readable pixels: proves data()/bytesPerRow map a
// live buffer (and that successive frames differ).
std::uint64_t checksum(const Cameras::CameraFrame& frame)
{
    const auto* data = frame.data();

    if (data == nullptr)
        return 0;

    auto sum = std::uint64_t {0};
    auto rowLength = (std::size_t) frame.width() * 4;

    for (auto y = 0; y < frame.height(); ++y)
    {
        const auto* row = data + (std::size_t) y * frame.bytesPerRow();

        for (std::size_t i = 0; i < rowLength; ++i)
            sum += row[i];
    }

    return sum;
}

void captureFrames(const Cameras::CameraDevice& device)
{
    auto formats = Cameras::Camera::supportedFormats(device);
    std::printf(
        "Supported formats for '%s' (%d):\n", device.name.c_str(), formats.size());

    for (auto i = 0; i < formats.size() && i < 8; ++i)
        std::printf("  %dx%d @ %.0f fps\n",
                    formats[i].width,
                    formats[i].height,
                    formats[i].maxFrameRate);

    std::printf("\n");

    auto camera = std::make_shared<Cameras::Camera>();
    auto frameCount = std::make_shared<std::atomic<int>>(0);
    auto savedFirst = std::make_shared<std::atomic<bool>>(false);

    camera->setFrameCallback(
        [frameCount, savedFirst](const Cameras::CameraFrame& frame)
        {
            auto n = ++(*frameCount);

            if (n <= 10)
                std::printf(
                    "  frame %2d: %dx%d  stride=%zu  t=%.3fs  checksum=%llu\n",
                    n,
                    frame.width(),
                    frame.height(),
                    frame.bytesPerRow(),
                    frame.timestampSeconds(),
                    (unsigned long long) checksum(frame));

            if (!savedFirst->exchange(true))
            {
                auto image = frame.toImage();

                if (image.isValid())
                {
                    try
                    {
                        image.save("/tmp/eacp_camera_frame.png");
                        std::printf(
                            "  saved first frame -> /tmp/eacp_camera_frame.png"
                            " (%dx%d)\n",
                            image.width(),
                            image.height());
                    }
                    catch (...)
                    {
                        std::printf("  (failed to save first frame)\n");
                    }
                }
            }
        });

    if (!camera->start({}))
    {
        std::printf("Failed to start capture.\n");
        return;
    }

    std::printf("Capturing... (up to 10 frames or 8s)\n");
    Threads::runEventLoopUntil([frameCount] { return frameCount->load() >= 10; },
                               Time::MS {8000});

    camera->stop();
    std::printf("\nDone. Total frames: %d\n", frameCount->load());
}

void runProbe()
{
    std::printf("eacp camera probe\n\n");

    auto devices = Cameras::Camera::devices();
    std::printf("Devices (%d):\n", devices.size());

    for (const auto& device: devices)
        std::printf("  - %s  [%s]%s\n",
                    device.name.c_str(),
                    device.id.c_str(),
                    device.isFrontFacing ? "  (front)" : "");

    if (devices.empty())
        std::printf("  (none found)\n");

    std::printf("\n");

    auto status = Cameras::Camera::permissionStatus();
    std::printf("Permission: %s\n\n", toString(status));

    if (status != Cameras::PermissionStatus::Granted)
    {
        std::printf("Camera access not granted. Grant it in System Settings >\n"
                    "Privacy & Security > Camera, then re-run to capture frames.\n");
        return;
    }

    if (devices.empty())
    {
        std::printf("No camera device available; nothing to capture.\n");
        return;
    }

    captureFrames(devices.front());
}
} // namespace

int main()
{
    return eacp::Apps::run(runProbe);
}
