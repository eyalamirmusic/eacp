#include <eacp/Graphics/Menu/Menu.h>
#include <eacp/VideoView/VideoView.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

using namespace eacp;

namespace
{
// The same clips Tests/Video decodes, fetched into a shared cache by CMake.
constexpr auto clipCount = 4;

const std::array<const char*, clipCount> clipNames {
    "bunny720.mp4", "heavy.mp4", "jellyfish.mp4", "sintel.mp4"};

// Bundled on macOS, copied next to the executable on Windows.
FilePath resolveClip(const std::string& name)
{
    auto bundled = Files::getBundleResourcePath(name);

    if (!bundled.empty() && File {FilePath {bundled}}.exists())
        return FilePath {bundled};

    return FilePath {"media/" + name};
}

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1280;
    options.height = 720;
    options.title = "eacp Video";
    options.flags = {Graphics::WindowFlags::Titled,
                     Graphics::WindowFlags::Closable,
                     Graphics::WindowFlags::Miniaturizable,
                     Graphics::WindowFlags::Resizable};
    return options;
}

// The video view plus an overlay drawn in the same GPU pass — a progress bar
// standing in for app chrome. It also self-reports render progress so the
// display path can be checked without eyes on the window.
struct DemoVideoView final : Video::VideoView
{
    void drawOverlay(Sprites::SpriteRenderer& renderer,
                     const Graphics::Rect& imageArea) override
    {
        ++overlayTicks;

        if (imageArea.w > 0.0f && imageArea.h > 0.0f)
            ++framesWithImage;

        if (player == nullptr || player->duration() <= 0.0)
            return;

        auto bounds = getLocalBounds();
        auto bar = Graphics::Rect {24.0f, bounds.h - 40.0f, bounds.w - 48.0f, 8.0f};

        renderer.fillRect(bar, {1.0f, 1.0f, 1.0f, 0.18f});

        auto fraction = std::clamp(
            (float) (player->currentTime() / player->duration()), 0.0f, 1.0f);

        renderer.fillRect(bar.withWidth(bar.w * fraction),
                          {0.35f, 0.8f, 1.0f, 0.95f});
    }

    Video::Player* player = nullptr;
    int overlayTicks = 0;
    int framesWithImage = 0;
};

struct VideoApp
{
    VideoApp()
    {
        // Force the CPU-upload display path (Windows uses it) for verification.
        if (getEnvValue("EACP_DEMO_UPLOAD_MODE") == "copy")
            view.setUploadMode(Video::UploadMode::Copy);

        player.onError = [](const std::string& message)
        { std::printf("video error: %s\n", message.c_str()); };

        player.setLooping(true);
        player.setMuted(true);

        view.player = &player;
        view.attach(player);
        window.setContentView(view);

        installMenuBar();
        selectClip(0);
        armAutoQuit();
    }

    ~VideoApp() { player.close(); }

    // The Clip menu, mirroring CameraViewDemo's Camera menu: checkable items
    // whose mark follows `selected` live, no rebuild on switch.
    void installMenuBar()
    {
        auto clipMenu = Graphics::Menu {"Clip"};

        for (auto index = 0; index < clipCount; ++index)
            clipMenu.add(Graphics::MenuItem::withCheckableAction(
                clipNames[(std::size_t) index],
                [this, index] { selectClip(index); },
                [this, index] { return selected == index; }));

        auto bar = Graphics::MenuBar {};
        bar.add(Graphics::standardApplicationMenu("eacp Video"));
        bar.add(std::move(clipMenu));

        Graphics::setApplicationMenuBar(bar, window);
    }

    // The view stays attached across the switch: it follows the Player object,
    // not the file the player happens to have open.
    void selectClip(int index)
    {
        selected = index;

        auto path = resolveClip(clipNames[(std::size_t) index]);

        if (!player.open(path))
        {
            std::printf("could not open %s\n", path.str().c_str());
            return;
        }

        player.play();
    }

    // Timer-driven, not render-driven, so it fires even when no frame ever
    // arrives and the on-arrival mode therefore never renders.
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

                std::printf("auto-quit: %d renders, %d with a video frame\n",
                            view.overlayTicks,
                            view.framesWithImage);
                Apps::quit();
            },
            100);
    }

    Video::Player player;
    DemoVideoView view;
    Graphics::Window window {makeOptions()};
    int selected = -1;
    std::optional<Threads::Timer> quitTimer;
    std::optional<Time::Deadline> quitDeadline;
};
} // namespace

int main()
{
    return eacp::Apps::run<VideoApp>();
}
