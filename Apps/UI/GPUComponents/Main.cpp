#include "Panels.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

// GPU components under stress: custom GPU rendering -- a lit mesh, compute
// particles, multisampled lines, a transparent plasma, a feedback loop over
// app-owned textures and a tile that only draws when touched -- each a
// UI::GPUComponent drawn into its own texture inside the host's one frame, and
// composited, clipped, faded and transformed like any other component.
//
// Grid puts up to 64 of them in a scrolling viewport, which is the culling
// test. Overlap stacks them with widgets, a popup, a captured group and a
// transformed tile. Churn adds, deletes and reparents them while they animate.
// The footer reads the host's own figures; its second line is the proof that a
// culled continuous tile keeps updating while it stops rendering.

using namespace eacp;
using namespace GPUComponentsDemo;

namespace
{
constexpr auto snapshotWidth = 1280.f;
constexpr auto snapshotHeight = 820.f;

std::string format(const char* pattern, double value)
{
    char text[48];
    std::snprintf(text, sizeof(text), pattern, value);
    return text;
}

struct DemoRoot final : UI::Component
{
    DemoRoot()
    {
        tabs.addTab("Grid", grid);
        tabs.addTab("Overlap", overlap);
        tabs.addTab("Churn", churn);
        tabs.setCurrentTabIndex(0);

        addChildren({toolbar, tabs, footer});
    }

    void resized() override
    {
        auto area = getLocalBounds();

        toolbar.setBounds(area.removeFromTop(40.f));
        footer.setBounds(area.removeFromBottom(44.f));
        tabs.setBounds(area);
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().background);
    }

    Toolbar toolbar;
    GridPanel grid;
    OverlapPanel overlap;
    ChurnPanel churn;
    UI::TabbedComponent tabs;
    StatsFooter footer;
};

struct DemoHost final : UI::ComponentHost
{
    using Clock = std::chrono::steady_clock;

    explicit DemoHost(bool live)
    {
        setFontPointSize(13.f);
        setRootComponent(root);

        auto onHostMaxFps = [this](int fps) { setAnimationMaxFps(fps); };
        auto onSnapshot = [this] { saveSnapshot(); };

        root.toolbar.onHostMaxFps = onHostMaxFps;
        root.toolbar.onSnapshot = onSnapshot;

        if (live)
        {
            auto refresh = [this] { refreshStats(); };
            statsTimer.create(refresh, 4);
        }
    }

    ~DemoHost() override { statsTimer.reset(); }

    void render(GPU::Frame& frame) override
    {
        auto start = Clock::now();
        UI::ComponentHost::render(frame);

        cpuSeconds += std::chrono::duration<double>(Clock::now() - start).count();
        ++framesRendered;
    }

    void refreshStats()
    {
        auto now = Clock::now();
        auto seconds =
            std::max(1e-3, std::chrono::duration<double>(now - lastStats).count());
        auto& counters = tileCounters();

        auto fps = (double) framesRendered / seconds;
        auto cpu = framesRendered > 0 ? cpuSeconds / framesRendered * 1000.0 : 0.0;
        auto gpu = GPU::Device::shared().lastFrameTimings().milliseconds;

        auto rate = [&](std::int64_t current, std::int64_t previous)
        { return format("%.0f/s", (double) (current - previous) / seconds); };

        auto first =
            format("%.1f fps", fps) + "   CPU " + format("%.2f ms", cpu) + "   GPU "
            + format("%.2f ms", gpu) + "   GPU components "
            + std::to_string(getLastRenderedGPUComponentCount()) + " rendered / "
            + std::to_string(getLastCulledGPUComponentCount()) + " culled / "
            + std::to_string(getLastHiddenGPUComponentCount()) + " hidden   layers "
            + std::to_string(getLastRenderedLayerCount()) + "   painted "
            + std::to_string(getLastPaintedComponentCount());

        auto second =
            "tile updates " + rate(counters.updates, lastCounters.updates) + ", "
            + rate(counters.offScreenUpdates, lastCounters.offScreenUpdates)
            + " off screen and still ticking   CPU work skipped "
            + rate(counters.skippedCpuWork, lastCounters.skippedCpuWork)
            + "   total updates " + std::to_string(counters.updates) + "   "
            + std::to_string(liveTileFrames().size()) + " tiles   " + status;

        root.footer.setLines(first, second);

        for (auto* frame: liveTileFrames())
            frame->refreshStats();

        lastStats = now;
        lastCounters = counters;
        framesRendered = 0;
        cpuSeconds = 0.0;
    }

    void saveSnapshot()
    {
        auto image = renderToImage(2.f);
        auto path = FilePath::tempDirectory() / "GPUComponents-snapshot-2x.png";

        if (!image.isValid())
        {
            status = "snapshot failed";
            return;
        }

        image.save(path);
        status = "saved " + path.str();
    }

    DemoRoot root;
    OwningPointer<Threads::Timer> statsTimer;
    Clock::time_point lastStats = Clock::now();
    TileCounters lastCounters;
    std::string status;
    double cpuSeconds = 0.0;
    int framesRendered = 0;
};

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = (int) snapshotWidth;
    options.height = (int) snapshotHeight;
    options.title = "eacp UI — GPU components";
    options.minWidth = 640;
    options.minHeight = 420;

    return options;
}

int snapshot(const char* path, int tab)
{
    auto host = DemoHost {false};
    host.setBounds({0.f, 0.f, snapshotWidth, snapshotHeight});
    host.resized();
    host.root.tabs.setCurrentTabIndex(tab);

    host.renderToImage(host.backingScale());
    host.refreshStats();

    auto image = host.renderToImage(host.backingScale());

    if (!image.isValid())
        return 1;

    image.save(FilePath {path});

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
        return snapshot(argv[1],
                        argc > 2 ? (int) std::strtol(argv[2], nullptr, 10) : 0);

    return Graphics::runWindowedApp<DemoHost>(makeOptions(), true);
}
