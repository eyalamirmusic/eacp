#include <eacp/UI/Network/OnlineResourceMonitor.h>
#include <eacp/UI/UI.h>

#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/StdPath.h>

#include <NanoTest/NanoTest.h>

#include <filesystem>

// The monitor over the registry, with nothing fetched: what it lists, what a
// click selects and what Clear does to the registry's directory. Nothing is
// rendered; the layout is what places the clicks.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
constexpr auto hostWidth = 600.f;
constexpr auto hostHeight = 400.f;

FilePath scratchDirectory(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path() / ("eacp-monitor-" + name);
    auto ignored = std::error_code {};
    std::filesystem::remove_all(path, ignored);
    return FilePath {path};
}

bool exists(const FilePath& path)
{
    return std::filesystem::exists(toStdPath(path));
}

OnlineResource::Info infoFor(const std::string& name, const std::string& url)
{
    auto info = OnlineResource::Info {};
    info.name = name;
    info.url = url;
    return info;
}

eacp::Graphics::MouseEvent mouseAt(Point position)
{
    auto event = eacp::Graphics::MouseEvent {};
    event.pos = position;
    event.downPos = position;
    return event;
}

// Points the registry at one case's folder and puts the previous one back,
// so a case leaves the directory as it found it whatever order they run in.
struct ScopedDirectory
{
    explicit ScopedDirectory(const FilePath& directory)
        : previous(OnlineResources::get().getDirectory())
    {
        OnlineResources::get().setDirectory(directory);
    }

    ~ScopedDirectory() { OnlineResources::get().setDirectory(previous); }

    FilePath previous;
};

struct Harness
{
    Harness()
    {
        host.setBounds({0.f, 0.f, hostWidth, hostHeight});
        host.setRootComponent(monitor);
    }

    void click(Point position)
    {
        host.mouseDown(mouseAt(position));
        host.mouseUp(mouseAt(position));
    }

    ComponentHost host;
    OnlineResourceMonitor monitor;
};

void forgetAll()
{
    auto& registry = OnlineResources::get();

    for (const auto& entry: registry.entries())
        registry.forget(entry.path);
}
} // namespace

auto tMonitorListsTheRegistry =
    test("OnlineResourceMonitor/listsTheRegistryAndFollowsIt") = []
{
    auto directory = scratchDirectory("lists");
    auto& registry = OnlineResources::get();
    auto scope = ScopedDirectory {directory};
    forgetAll();

    registry.declare(infoFor("First", "https://host/first.bin"));
    registry.declare(infoFor("Second", "https://host/second.bin"));

    auto harness = Harness {};
    const auto& rows = harness.monitor.getRows();

    check(rows.size() == 2);
    check(rows[0].info.name == "First");
    check(rows[1].info.name == "Second");
    check(rows[0].directory == directory);
    check(!harness.monitor.getSelectedEntry().has_value());

    registry.declare(infoFor("Third", "https://host/third.bin"));
    harness.monitor.refresh();
    check(harness.monitor.getRows().size() == 3);

    forgetAll();
};

auto tMonitorClickSelects = test("OnlineResourceMonitor/clickingARowSelectsIt") = []
{
    auto directory = scratchDirectory("select");
    auto& registry = OnlineResources::get();
    auto scope = ScopedDirectory {directory};
    forgetAll();

    registry.declare(infoFor("First", "https://host/first.bin"));
    registry.declare(infoFor("Second", "https://host/second.bin"));

    auto harness = Harness {};
    check(harness.monitor.getRows().size() == 2);
    check(!harness.monitor.getSelectedEntry().has_value());

    // The second row: rows start under the two-line header.
    harness.click({hostWidth * 0.5f, 12.f + 44.f + 48.f + 24.f});

    auto selected = harness.monitor.getSelectedEntry();
    check(selected.has_value());
    check(selected->info.name == "Second");

    forgetAll();
};

auto tMonitorClearDeletesTheFolder =
    test("OnlineResourceMonitor/clearAllDeletesTheDirectory") = []
{
    auto directory = scratchDirectory("clear");
    auto& registry = OnlineResources::get();
    auto scope = ScopedDirectory {directory};
    forgetAll();

    std::filesystem::create_directories(toStdPath(directory));
    auto stale = std::string {"stale"};
    Files::writeFile(
        directory / "stale.bin",
        {reinterpret_cast<const std::uint8_t*>(stale.data()), (int) stale.size()});

    registry.declare(infoFor("Stale", "https://host/stale.bin"));

    auto harness = Harness {};
    check(exists(directory));

    harness.monitor.clearAll();

    check(!exists(directory));
    check(harness.monitor.getRows().size() == 1);
    check(!harness.monitor.getRows()[0].available);

    forgetAll();
};
