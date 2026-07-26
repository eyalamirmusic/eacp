#include "Catalogue.h"
#include "Downloader.h"

#include <eacp/Text/TextRenderer.h>
#include <eacp/VideoView/VideoView.h>

#include <cstdio>

using namespace eacp;
using namespace VideoDemo;

namespace
{
constexpr auto windowWidth = 900;
constexpr auto windowHeight = 620;

constexpr auto margin = 24.0f;
constexpr auto rowHeight = 62.0f;
constexpr auto rowGap = 8.0f;

std::string formatted(const char* format, ...)
{
    char buffer[512] = {};

    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    return buffer;
}

std::string megabytes(std::int64_t bytes)
{
    return formatted("%.1f MB", (double) bytes / (1024.0 * 1024.0));
}

FilePath cachePathFor(const std::string& fileName)
{
    return FilePath::cacheDirectory() / fileName;
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = windowWidth;
    options.height = windowHeight;
    options.title = "eacp Video — download and play";
    return options;
}
} // namespace

// Offers the clips listed in the embedded Clips.json, fetches the one you pick,
// and plays it through the same VideoView as the local-file sample.
//
// This class is only the UI: the catalogue is parsed by Catalogue.h out of a
// resource embedded at build time, and the transfer — thread, progress,
// cancellation, the partial-file dance — belongs to Downloader.
struct BrowserView final : Video::VideoView
{
    enum class Mode
    {
        Browsing,
        Downloading,
        Playing,
        Failed
    };

    BrowserView() { setHandlesMouseEvents(true); }

    // Unhooks from the stream while it is still alive. The base destructor
    // cannot do this: by the time it runs, `stream` below has already gone.
    ~BrowserView() override { detach(); }

    const Vector<Clip>& clips() const { return catalogue().clips; }

    Graphics::Rect rowArea(int index) const
    {
        auto bounds = getLocalBounds();
        auto top = margin + 64.0f + (float) index * (rowHeight + rowGap);
        return {margin, top, bounds.w - margin * 2.0f, rowHeight};
    }

    Graphics::Rect scrubArea() const
    {
        auto bounds = getLocalBounds();
        return {margin, bounds.h - 40.0f, bounds.w - margin * 2.0f, 24.0f};
    }

    Graphics::Rect backArea() const { return {margin, margin, 90.0f, 28.0f}; }

    void update(Threads::FrameTime frameTime) override
    {
        VideoView::update(frameTime);

        if (frameTime.delta > 0.0)
        {
            auto instant = 1.0 / frameTime.delta;
            smoothedFps =
                smoothedFps > 0.0 ? smoothedFps * 0.9 + instant * 0.1 : instant;
        }
    }

    void start(int index)
    {
        if (index < 0 || index >= clips().size())
            return;

        const auto& clip = clips()[index];
        startUrl(clip.url, clip.name, cachePathFor(clip.fileName));
    }

    // Any H.264 MP4 the platform decoder can read. The catalogue is a
    // convenience, not a limit — a 4K clip is one command-line argument away.
    void startUrl(const std::string& url,
                  const std::string& name,
                  const FilePath& path)
    {
        if (mode == Mode::Downloading)
            return;

        title = name;

        if (File {path}.exists())
        {
            play(path);
            return;
        }

        mode = Mode::Downloading;
        message = "Downloading " + name;
        setContinuous(true); // keep the progress bar moving

        downloader.start(url,
                         path,
                         [this](Downloader::Result result)
                         {
                             if (result.cancelled)
                                 showCatalogue();
                             else if (result.ok)
                                 play(result.path);
                             else
                                 fail(result.error);
                         });
    }

    void play(const FilePath& path)
    {
        if (!stream.open(path))
        {
            fail("The file downloaded but could not be decoded.");
            return;
        }

        mode = Mode::Playing;
        setFit(Fit::Contain);
        attach(player);
        player.setLooping(true);
        player.play();
        repaint();
    }

    void fail(const std::string& reason)
    {
        mode = Mode::Failed;
        message = reason;
        setContinuous(false);
        repaint();
    }

    void showCatalogue()
    {
        detach();
        stream.close();

        mode = Mode::Browsing;
        message.clear();
        setContinuous(false);
        repaint();
    }

    void drawCatalogue(Sprites::SpriteRenderer& renderer)
    {
        auto lineStep = text.lineHeight();

        text.draw("Pick a clip to download and play",
                  {margin, margin + lineStep},
                  {0.95f, 0.97f, 1.0f});
        text.draw("Nothing is fetched until you choose one. Files are cached, so "
                  "a second run plays straight away.",
                  {margin, margin + lineStep * 2.2f},
                  {0.55f, 0.62f, 0.72f});

        for (auto index = 0; index < clips().size(); ++index)
        {
            const auto& clip = clips()[index];
            auto area = rowArea(index);
            auto cached = File {cachePathFor(clip.fileName)}.exists();
            auto hot = index == hovered;

            renderer.fillRect(area,
                              hot ? Graphics::Color {0.16f, 0.24f, 0.34f, 1.0f}
                                  : Graphics::Color {0.10f, 0.13f, 0.18f, 1.0f});
            renderer.drawRect(area,
                              hot ? Graphics::Color {0.35f, 0.65f, 0.95f, 0.9f}
                                  : Graphics::Color {1.0f, 1.0f, 1.0f, 0.12f},
                              1.0f);

            text.draw(
                clip.name, {area.x + 14.0f, area.y + 24.0f}, {0.95f, 0.97f, 1.0f});
            text.draw(clip.detail,
                      {area.x + 14.0f, area.y + 44.0f},
                      {0.58f, 0.66f, 0.76f});

            auto label = cached ? "cached — plays now" : "click to download";
            auto labelColor = cached ? Graphics::Color {0.45f, 0.85f, 0.55f}
                                     : Graphics::Color {0.55f, 0.70f, 0.90f};

            text.draw(
                label,
                {area.x + area.w - text.measure(label) - 14.0f, area.y + 24.0f},
                labelColor);
        }

        if (mode == Mode::Failed)
            text.draw(message,
                      {margin, rowArea(clips().size()).y + 24.0f},
                      {1.0f, 0.55f, 0.5f});
    }

    void drawDownload(Sprites::SpriteRenderer& renderer)
    {
        auto bounds = getLocalBounds();
        auto received = downloader.bytesReceived();
        auto total = downloader.totalBytes();
        auto fraction = downloader.fraction();

        auto bar = Graphics::Rect {
            margin, bounds.h * 0.5f - 12.0f, bounds.w - margin * 2.0f, 24.0f};

        renderer.fillRect(bar, {0.10f, 0.13f, 0.18f, 1.0f});

        auto filled = bar;
        filled.w = bar.w * std::max(0.0f, fraction);
        renderer.fillRect(filled, {0.25f, 0.7f, 1.0f, 0.95f});
        renderer.drawRect(bar, {1.0f, 1.0f, 1.0f, 0.25f}, 1.0f);

        text.draw(message, {margin, bar.y - 16.0f}, {0.95f, 0.97f, 1.0f});

        // A server that declares no length gives no percentage to show, only
        // how much has arrived.
        auto status = fraction >= 0.0f
                          ? formatted("%s of %s  (%.0f%%)",
                                      megabytes(received).c_str(),
                                      megabytes(total).c_str(),
                                      fraction * 100.0f)
                          : formatted("%s so far", megabytes(received).c_str());

        text.draw(status, {margin, bar.y + bar.h + 22.0f}, {0.6f, 0.68f, 0.78f});
        text.draw("Click anywhere to cancel",
                  {margin, bar.y + bar.h + 44.0f},
                  {0.45f, 0.52f, 0.62f});
    }

    void drawPlayback(Sprites::SpriteRenderer& renderer)
    {
        const auto& info = stream.info();
        auto stats = stream.stats();
        auto duration = info.duration;
        auto fraction =
            duration > 0.0 ? (float) (player.position() / duration) : 0.0f;

        auto bar = scrubArea();
        renderer.fillRect(bar, {0.0f, 0.0f, 0.0f, 0.5f});

        auto played = bar;
        played.w = bar.w * fraction;
        renderer.fillRect(played, {0.25f, 0.7f, 1.0f, 0.9f});
        renderer.drawRect(bar, {1.0f, 1.0f, 1.0f, 0.3f}, 1.0f);

        auto back = backArea();
        renderer.fillRect(back, {0.0f, 0.0f, 0.0f, 0.55f});
        renderer.drawRect(back, {1.0f, 1.0f, 1.0f, 0.25f}, 1.0f);
        text.draw("< back", {back.x + 14.0f, back.y + 19.0f}, {0.9f, 0.94f, 1.0f});

        auto hud = formatted("%s   %dx%d %.0ffps   render %.0f fps   "
                             "decoded %llu  skipped %llu   queue %d/%d   %s",
                             title.c_str(),
                             info.width,
                             info.height,
                             info.frameRate,
                             smoothedFps,
                             (unsigned long long) stats.decoded,
                             (unsigned long long) stats.skipped,
                             stats.queued,
                             stats.depth,
                             lastFrameWasZeroCopy() ? "zero-copy" : "cpu copy");

        auto panel = Graphics::Rect {
            back.x + back.w + 10.0f, back.y, text.measure(hud) + 24.0f, back.h};
        renderer.fillRect(panel, {0.0f, 0.0f, 0.0f, 0.55f});
        text.draw(hud, {panel.x + 12.0f, panel.y + 19.0f}, {0.82f, 0.92f, 1.0f});
    }

    void drawOverlay(GPU::RenderPass& pass,
                     Sprites::SpriteRenderer& renderer,
                     const Graphics::Rect&) override
    {
        auto bounds = getLocalBounds();
        text.setViewport({bounds.w, bounds.h}, backingScale());
        text.begin();

        switch (mode)
        {
            case Mode::Downloading:
                drawDownload(renderer);
                break;
            case Mode::Playing:
                drawPlayback(renderer);
                break;
            default:
                drawCatalogue(renderer);
                break;
        }

        // After every sprite draw, so the text lands on top of the panels.
        text.flush(pass);
    }

    int rowAt(Graphics::Point point) const
    {
        for (auto index = 0; index < clips().size(); ++index)
            if (rowArea(index).contains(point))
                return index;

        return -1;
    }

    void mouseMoved(const Graphics::MouseEvent& event) override
    {
        if (mode != Mode::Browsing && mode != Mode::Failed)
            return;

        auto row = rowAt(event.pos);

        if (row != hovered)
        {
            hovered = row;
            repaint();
        }
    }

    void mouseExited(const Graphics::MouseEvent&) override
    {
        hovered = -1;
        repaint();
    }

    void mouseDown(const Graphics::MouseEvent& event) override
    {
        if (mode == Mode::Downloading)
        {
            downloader.cancel();
            return;
        }

        if (mode == Mode::Playing)
        {
            if (backArea().contains(event.pos))
                showCatalogue();
            else if (scrubArea().contains(event.pos))
                scrubTo(event.pos.x);
            else if (player.isPlaying())
                player.pause();
            else
                player.play();

            return;
        }

        if (auto row = rowAt(event.pos); row >= 0)
            start(row);
    }

    void mouseDragged(const Graphics::MouseEvent& event) override
    {
        if (mode == Mode::Playing && scrubArea().contains(event.pos))
            scrubTo(event.pos.x);
    }

    void scrubTo(float x)
    {
        auto bar = scrubArea();
        auto duration = stream.info().duration;

        if (bar.w <= 0.0f || duration <= 0.0)
            return;

        player.setPosition(duration * std::clamp((x - bar.x) / bar.w, 0.0f, 1.0f));
        repaint();
    }

    Video::FrameStream stream;
    Video::Player player {stream};
    Text::TextRenderer text {13.0f};
    Downloader downloader;

    Mode mode = Mode::Browsing;
    std::string message;
    std::string title;
    int hovered = -1;
    double smoothedFps = 0.0;
};

struct DownloadApp
{
    DownloadApp()
    {
        window.setContentView(view);
        startFromCommandLine();
    }

    // `DownloadAndPlay <index>` picks a catalogue entry, `DownloadAndPlay <url>`
    // fetches anything else — which is how to point this at a 4K clip, since no
    // free 4K H.264 source was dependable enough to put in the list.
    void startFromCommandLine()
    {
        const auto& args = Apps::getAppEnvironment().commandLineArgs;

        if (args.size() < 2)
            return;

        const auto& argument = args[1];

        if (argument.find("://") == std::string::npos)
        {
            view.start((int) std::strtol(argument.c_str(), nullptr, 10));
            return;
        }

        auto name = Files::filenameFromPath(argument);
        view.startUrl(argument, name, FilePath::cacheDirectory() / name);
    }

    BrowserView view;
    Graphics::Window window {windowOptions()};
};

int main(int argc, char* argv[])
{
    return eacp::Apps::run<DownloadApp>(argc, argv);
}
