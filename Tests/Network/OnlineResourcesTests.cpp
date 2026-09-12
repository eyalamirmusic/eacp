#include "Common.h"

#include <eacp/Core/Utils/StdPath.h>

#include <algorithm>
#include <filesystem>

// The registry every OnlineResource reports into. These share the process
// with every other suite's fetches, so each case names its own directory and
// looks its entries up by path rather than counting what is listed.

using namespace nano;
using eacp::FilePath;
using eacp::OnlineResource;
using eacp::OnlineResources;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::HTTP::ServerOptions;
using eacp::HTTP::ServerThreadingMode;

namespace
{
constexpr auto fetchTimeout = eacp::Time::MS {5000};
using Status = OnlineResources::Status;

std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

FilePath scratchDirectory(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path() / ("eacp-registry-" + name);
    auto ignored = std::error_code {};
    std::filesystem::remove_all(path, ignored);
    return FilePath {path};
}

bool exists(const FilePath& path)
{
    return std::filesystem::exists(eacp::toStdPath(path));
}

struct StaticFileServer
{
    explicit StaticFileServer(std::string bodyToUse)
        : body(std::move(bodyToUse))
    {
        check(server.listen(0,
                            [this](const Request&)
                            {
                                auto response = Response();
                                response.statusCode = 200;
                                response.content = body;
                                response.setHeader("ETag", "\"v1\"");
                                return response;
                            }));
    }

    ~StaticFileServer() { server.stop(); }

    std::string url(const std::string& file) const
    {
        return baseUrl(server.boundPort()) + "/" + file;
    }

    Server server;
    std::string body;
};

OnlineResource::Info infoFor(const std::string& name, const std::string& url)
{
    auto info = OnlineResource::Info {};
    info.name = name;
    info.url = url;
    return info;
}

OnlineResources::Entry entryAt(const FilePath& path)
{
    auto entry = OnlineResources::get().find(path);
    check(entry.has_value());
    return entry.value_or(OnlineResources::Entry {});
}

// Notifications land one loop turn after the change.
void pump()
{
    eacp::Threads::runEventLoopFor(eacp::Time::MS {50});
}

void waitUntilNotFetching(const FilePath& path)
{
    auto deadline = eacp::Time::Deadline {fetchTimeout};

    while (entryAt(path).isFetching() && !deadline.expired())
        eacp::Threads::runEventLoopFor(eacp::Time::MS {20});
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

// Registered for one case and gone with it, so a listener never outlives
// the counter it writes to.
struct ChangeCounter
{
    ChangeCounter()
        : id(OnlineResources::get().addListener([this] { ++changes; }))
    {
    }

    ~ChangeCounter() { OnlineResources::get().removeListener(id); }

    OnlineResources::ListenerId id;
    int changes = 0;
};
} // namespace

auto tRegistryDeclare =
    test("OnlineResources/declareListsAResourceNotYetOnDisk") = []
{
    auto directory = scratchDirectory("declare");
    auto counter = ChangeCounter {};
    auto& registry = OnlineResources::get();
    auto scope = ScopedDirectory {directory};

    registry.declare(infoFor("Data", "https://host/files/data.bin"));

    auto entry = entryAt(directory / "data.bin");
    check(entry.info.name == "Data");
    check(entry.directory == directory);
    check(registry.getDirectory() == directory);
    check(OnlineResource::defaultDirectory() == directory);
    check(!entry.available);
    check(entry.sizeOnDisk == 0);
    check(entry.status == Status::idle);
    check(entry.error.empty());

    pump();
    check(counter.changes == 1);

    registry.forget(entry.path);
    check(!registry.find(entry.path).has_value());
};

auto tFetchReports =
    test("OnlineResources/aFetchIsListedFetchingThenFetchedWithItsSize") = []
{
    auto server = StaticFileServer {std::string(65536, 'a')};
    auto directory = scratchDirectory("fetch");
    auto& registry = OnlineResources::get();
    auto scope = ScopedDirectory {directory};
    auto counter = ChangeCounter {};

    registry.declare(infoFor("Data", server.url("data.bin")));
    auto path = directory / "data.bin";

    auto pending = registry.fetch(path);
    check(entryAt(path).status == Status::fetching);
    check(entryAt(path).isFetching());

    auto result = pending.waitFor(fetchTimeout);
    check(result.ok);

    auto entry = entryAt(path);
    check(entry.status == Status::fetched);
    check(entry.available);
    check(entry.sizeOnDisk == 65536);
    check(entry.progress.stage == OnlineResource::Progress::Stage::done);
    check(entry.progress.bytesReceived == 65536);
    check(entry.error.empty());

    pump();
    check(counter.changes >= 2);

    // Not declared, so nothing to fetch.
    auto refused = registry.fetch(directory / "nothing.bin").waitFor(fetchTimeout);
    check(!refused.ok);
    check(!refused.error.empty());
};

auto tRegistryAppOwned =
    test("OnlineResources/anObjectTheAppOwnsReportsTheSameWay") = []
{
    auto server = StaticFileServer {"payload"};
    auto directory = scratchDirectory("owned");
    auto& registry = OnlineResources::get();

    auto resource =
        OnlineResource {infoFor("Own", server.url("own.txt")), directory};
    check(!registry.find(resource.path()).has_value());

    check(resource.start().waitFor(fetchTimeout).ok);

    auto entry = entryAt(resource.path());
    check(entry.status == Status::fetched);
    check(entry.available);
    check(entry.sizeOnDisk == 7);

    // A second start finds the copy and asks the server: still one entry,
    // still fetched.
    check(resource.start().waitFor(fetchTimeout).ok);
    auto listed = registry.entries();
    check(std::count_if(listed.begin(),
                        listed.end(),
                        [&](const OnlineResources::Entry& entry)
                        { return entry.directory == directory; })
          == 1);
    check(entryAt(resource.path()).status == Status::fetched);
};

auto tDestroyedStillReports =
    test("OnlineResources/anObjectDestroyedMidFetchStillReportsTheOutcome") = []
{
    auto gate = StallGate {};
    auto options = ServerOptions {};
    options.threading = ServerThreadingMode::ThreadPool;

    auto server = Server {options};
    check(server.listen(0,
                        [&](const Request&)
                        {
                            gate.wait();
                            auto response = Response();
                            response.statusCode = 200;
                            response.content = "late";
                            return response;
                        }));

    auto directory = scratchDirectory("destroyed");
    auto resource = eacp::makeOwned<OnlineResource>(
        infoFor("Late", baseUrl(server.boundPort()) + "/late.txt"), directory);
    auto path = resource->path();

    resource->start();
    check(entryAt(path).isFetching());

    resource.reset();
    gate.release();
    waitUntilNotFetching(path);

    // Destroying the object cancels the transfer, and the registry learns
    // that rather than being left showing a fetch that never ends.
    auto entry = entryAt(path);
    check(entry.status == Status::cancelled);
    check(!entry.available);

    server.stop();
};

auto tRegistryFailure = test("OnlineResources/aFailedFetchKeepsItsError") = []
{
    auto server = Server();
    check(server.listen(0,
                        [](const Request&)
                        {
                            auto response = Response();
                            response.statusCode = 404;
                            return response;
                        }));

    auto directory = scratchDirectory("failed");
    auto& registry = OnlineResources::get();
    auto scope = ScopedDirectory {directory};

    registry.declare(
        infoFor("Missing", baseUrl(server.boundPort()) + "/missing.bin"));
    auto path = directory / "missing.bin";

    auto result = registry.fetch(path).waitFor(fetchTimeout);
    check(!result.ok);

    auto entry = entryAt(path);
    check(entry.status == Status::failed);
    check(entry.error == "HTTP 404");
    check(!entry.available);

    server.stop();
};

auto tRegistryCancel =
    test("OnlineResources/cancelThroughTheRegistryIsListedCancelled") = []
{
    auto server = StaticFileServer {std::string(1 << 20, 'z')};
    auto directory = scratchDirectory("cancel");
    auto& registry = OnlineResources::get();
    auto scope = ScopedDirectory {directory};

    registry.declare(infoFor("Big", server.url("big.bin")));
    auto path = directory / "big.bin";

    auto pending = registry.fetch(path);
    registry.cancel(path);

    auto result = pending.waitFor(fetchTimeout);
    check(result.cancelled);

    auto entry = entryAt(path);
    check(entry.status == Status::cancelled);
    check(!entry.available);
    check(!exists(path));
};

auto tRegistryRemove =
    test("OnlineResources/removeDeletesTheCopyAndKeepsTheEntry") = []
{
    auto server = StaticFileServer {"payload"};
    auto directory = scratchDirectory("remove");
    auto& registry = OnlineResources::get();
    auto scope = ScopedDirectory {directory};

    registry.declare(infoFor("Data", server.url("data.txt")));
    auto path = directory / "data.txt";
    check(registry.fetch(path).waitFor(fetchTimeout).ok);
    check(entryAt(path).available);

    check(registry.remove(path));
    check(!exists(path));
    check(!exists(FilePath {path.str() + ".resource.json"}));

    auto entry = entryAt(path);
    check(!entry.available);
    check(entry.sizeOnDisk == 0);
    check(entry.status == Status::fetched);

    // Fetchable again, into the same entry.
    check(registry.fetch(path).waitFor(fetchTimeout).downloaded);
    check(entryAt(path).available);
};

auto tRegistryClear =
    test("OnlineResources/clearDeletesTheFolderButNotUnderATransfer") = []
{
    auto gate = StallGate {};
    auto options = ServerOptions {};
    options.threading = ServerThreadingMode::ThreadPool;

    auto server = Server {options};
    check(server.listen(0,
                        [&](const Request& request)
                        {
                            if (request.url.find("slow") != std::string::npos)
                                gate.wait();

                            auto response = Response();
                            response.statusCode = 200;
                            response.content = "body";
                            return response;
                        }));

    auto directory = scratchDirectory("clear");
    auto elsewhere = scratchDirectory("clear-elsewhere");
    auto& registry = OnlineResources::get();
    auto scope = ScopedDirectory {directory};
    auto url = baseUrl(server.boundPort());

    registry.declare(infoFor("Quick", url + "/quick.txt"));
    registry.declare(infoFor("Slow", url + "/slow.txt"));

    // Told its own folder, so listed but not under the one clear() empties.
    auto other = OnlineResource {infoFor("Other", url + "/other.txt"), elsewhere};

    check(registry.fetch(directory / "quick.txt").waitFor(fetchTimeout).ok);
    check(other.start().waitFor(fetchTimeout).ok);

    auto slow = registry.fetch(directory / "slow.txt");
    check(registry.isAnythingFetching());
    check(!registry.clear());
    check(exists(directory / "quick.txt"));

    gate.release();
    check(slow.waitFor(fetchTimeout).ok);
    check(!registry.isAnythingFetching());

    check(registry.clear());
    check(!exists(directory));
    check(!entryAt(directory / "quick.txt").available);
    check(!entryAt(directory / "slow.txt").available);

    check(entryAt(elsewhere / "other.txt").available);
    check(exists(elsewhere / "other.txt"));

    server.stop();
};
