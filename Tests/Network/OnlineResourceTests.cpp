#include "Common.h"

#include <eacp/Core/Utils/StdPath.h>
#include <eacp/Core/Utils/Zip.h>

#include <atomic>
#include <filesystem>
#include <memory>

using namespace nano;
using eacp::FilePath;
using eacp::OnlineResource;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::HTTP::ServerOptions;
using eacp::HTTP::ServerThreadingMode;

namespace
{
constexpr auto fetchTimeout = eacp::Time::MS {5000};

std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

FilePath scratchDirectory(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path() / ("eacp-resource-" + name);
    auto ignored = std::error_code {};
    std::filesystem::remove_all(path, ignored);
    return FilePath {path};
}

std::string readAll(const FilePath& path)
{
    return eacp::Files::readFile(path);
}

bool exists(const FilePath& path)
{
    return std::filesystem::exists(eacp::toStdPath(path));
}

int entriesIn(const FilePath& directory)
{
    auto count = 0;
    auto ignored = std::error_code {};

    for ([[maybe_unused]] const auto& entry:
         std::filesystem::directory_iterator(eacp::toStdPath(directory), ignored))
        ++count;

    return count;
}

Response okResponse(const std::string& body)
{
    auto response = Response();
    response.statusCode = 200;
    response.content = body;
    return response;
}

// A server that hands out one body under one ETag and answers 304 to a
// matching If-None-Match, counting what it was asked.
struct StaticFileServer
{
    StaticFileServer(std::string bodyToUse, std::string etagToUse = {})
        : body(std::move(bodyToUse))
        , etag(std::move(etagToUse))
    {
        check(server.listen(
            0, [this](const Request& request) { return answer(request); }));
    }

    ~StaticFileServer() { server.stop(); }

    Response answer(const Request& request)
    {
        ++requests;

        if (request.hasHeader("If-None-Match"))
            ++conditionalRequests;

        if (!etag.empty() && request.getHeader("If-None-Match") == etag)
        {
            auto response = Response();
            response.statusCode = 304;
            response.setHeader("ETag", etag);
            return response;
        }

        auto response = okResponse(body);

        if (!etag.empty())
            response.setHeader("ETag", etag);

        return response;
    }

    std::string url(const std::string& file) const
    {
        return baseUrl(server.boundPort()) + "/" + file;
    }

    Server server;
    std::string body;
    std::string etag;
    std::atomic<int> requests {0};
    std::atomic<int> conditionalRequests {0};
};

OnlineResource::Result fetchNow(OnlineResource& resource)
{
    return resource.start().waitFor(fetchTimeout);
}
} // namespace

auto tDefaultDirectory =
    test("OnlineResource/defaultDirectoryIsUnderAppSupport") = []
{
    auto directory = OnlineResource::defaultDirectory();
    auto support = FilePath::appSupportDirectory();

    check(directory.str().starts_with(support.str()));
    check(directory.str().ends_with("/Resources"));
};

auto tFileNameFromUrl = test("OnlineResource/fileNameDefaultsToUrlLastSegment") = []
{
    auto resource = OnlineResource {{"Clip", "https://host/videos/clip.mp4?token=1"},
                                    FilePath {"/tmp/r"}};

    check(resource.info().fileName == "clip.mp4");
    check(resource.path() == FilePath {"/tmp/r/clip.mp4"});

    auto zip =
        OnlineResource {{"Pack", "https://host/pack.zip"}, FilePath {"/tmp/r"}};
    check(zip.path() == FilePath {"/tmp/r/pack"});
};

auto tDownloadsAFile = test("OnlineResource/downloadsAndRecordsAFile") = []
{
    auto server = StaticFileServer {std::string(4096, 'x'), "\"v1\""};
    auto directory = scratchDirectory("download");

    auto resource = OnlineResource {{"Data", server.url("data.bin")}, directory};
    check(!resource.isAvailable());

    auto result = fetchNow(resource);
    check(result.ok);
    check(result.downloaded);
    check(!result.cancelled);
    check(result.error.empty());
    check(result.path == directory / "data.bin");
    check(readAll(result.path) == server.body);
    check(resource.isAvailable());
    check(!resource.isRunning());
    check(resource.progress().stage == OnlineResource::Progress::Stage::done);

    check(!exists(directory / "data.bin.part"));
    check(exists(directory / "data.bin.resource.json"));
    check(entriesIn(directory) == 2);
};

auto tUnpacksAZip = test("OnlineResource/unpacksAZipIntoAFolder") = []
{
    auto writer = eacp::Zip::Writer {};
    check(writer.add("hello.txt", "hello, zip"));
    check(writer.add("nested/data.txt", "nested"));
    auto archive = writer.finish();

    auto server = StaticFileServer {std::string(archive.begin(), archive.end())};
    auto directory = scratchDirectory("zip");

    auto resource = OnlineResource {{"Pack", server.url("pack.zip")}, directory};
    auto result = fetchNow(resource);

    check(result.ok);
    check(result.path == directory / "pack");
    check(readAll(directory / "pack" / "hello.txt") == "hello, zip");
    check(readAll(directory / "pack" / "nested" / "data.txt") == "nested");
    check(!exists(directory / "pack.zip"));
    check(!exists(directory / "pack.zip.part"));
    check(!exists(directory / "pack.extracting"));
    check(resource.isAvailable());
};

auto tCorruptZipFails = test("OnlineResource/aCorruptZipFailsAndLeavesNothing") = []
{
    auto server = StaticFileServer {"this is not a zip"};
    auto directory = scratchDirectory("badzip");

    auto resource = OnlineResource {{"Pack", server.url("pack.zip")}, directory};
    auto result = fetchNow(resource);

    check(!result.ok);
    check(!result.error.empty());
    check(!exists(directory / "pack"));
    check(entriesIn(directory) == 0);
};

auto tRevalidates = test("OnlineResource/revalidatesWithETagAndKeepsCopyOn304") = []
{
    auto server = StaticFileServer {"first", "\"v1\""};
    auto directory = scratchDirectory("etag");

    auto resource = OnlineResource {{"Data", server.url("data.txt")}, directory};
    check(fetchNow(resource).downloaded);
    check(server.requests == 1);

    auto again = fetchNow(resource);
    check(again.ok);
    check(!again.downloaded);
    check(server.requests == 2);
    check(server.conditionalRequests == 1);
    check(readAll(again.path) == "first");
    check(!exists(directory / "data.txt.part"));

    server.body = "second";
    server.etag = "\"v2\"";

    auto replaced = fetchNow(resource);
    check(replaced.ok);
    check(replaced.downloaded);
    check(server.requests == 3);
    check(readAll(replaced.path) == "second");
};

auto tTrustSkipsServer = test("OnlineResource/trustNeverAsksTheServerAgain") = []
{
    auto server = StaticFileServer {"first", "\"v1\""};
    auto directory = scratchDirectory("trust");

    auto resource = OnlineResource {{"Data", server.url("data.txt")},
                                    directory,
                                    OnlineResource::Freshness::trust};

    check(fetchNow(resource).downloaded);

    auto again = fetchNow(resource);
    check(again.ok);
    check(!again.downloaded);
    check(server.requests == 1);
};

auto tNoValidators = test("OnlineResource/noValidatorsMeansNoSecondRequest") = []
{
    auto server = StaticFileServer {"first"};
    auto directory = scratchDirectory("novalidators");

    auto resource = OnlineResource {{"Data", server.url("data.txt")}, directory};
    check(fetchNow(resource).downloaded);

    auto again = fetchNow(resource);
    check(again.ok);
    check(!again.downloaded);
    check(server.requests == 1);
};

auto tVersionBump = test("OnlineResource/aVersionBumpRedownloadsWithoutAsking") = []
{
    auto server = StaticFileServer {"first", "\"v1\""};
    auto directory = scratchDirectory("version");

    {
        auto info = OnlineResource::Info {"Data", server.url("data.txt")};
        info.version = "1";
        auto resource = OnlineResource {info, directory};
        check(fetchNow(resource).downloaded);
    }

    auto info = OnlineResource::Info {"Data", server.url("data.txt")};
    info.version = "2";
    auto resource = OnlineResource {info, directory};
    check(!resource.isAvailable());

    auto result = fetchNow(resource);
    check(result.ok);
    check(result.downloaded);
    check(server.requests == 2);
    check(server.conditionalRequests == 0);
    check(resource.isAvailable());
};

auto tUrlChange = test("OnlineResource/aDifferentUrlToTheSameFileRedownloads") = []
{
    auto server = StaticFileServer {"first", "\"v1\""};
    auto directory = scratchDirectory("url");

    {
        auto resource =
            OnlineResource {{"Data", server.url("a/data.txt")}, directory};
        check(fetchNow(resource).downloaded);
    }

    auto resource = OnlineResource {{"Data", server.url("b/data.txt")}, directory};
    check(!resource.isAvailable());
    check(fetchNow(resource).downloaded);
    check(server.conditionalRequests == 0);
};

auto tMissingSidecar = test("OnlineResource/aCopyWithoutASidecarIsReplaced") = []
{
    auto server = StaticFileServer {"fresh"};
    auto directory = scratchDirectory("nosidecar");

    std::filesystem::create_directories(eacp::toStdPath(directory));
    auto stale = std::string {"stale"};
    eacp::Files::writeFile(
        directory / "data.txt",
        {reinterpret_cast<const std::uint8_t*>(stale.data()), (int) stale.size()});

    auto resource = OnlineResource {{"Data", server.url("data.txt")}, directory};
    check(!resource.isAvailable());

    auto result = fetchNow(resource);
    check(result.downloaded);
    check(readAll(result.path) == "fresh");
};

auto tHttpError = test("OnlineResource/anHttpErrorLeavesNothingBehind") = []
{
    auto server = Server();
    check(server.listen(0,
                        [](const Request&)
                        {
                            auto response = Response();
                            response.statusCode = 404;
                            response.content = "nope";
                            return response;
                        }));

    auto directory = scratchDirectory("404");
    auto resource = OnlineResource {
        {"Data", baseUrl(server.boundPort()) + "/missing.bin"}, directory};

    auto result = fetchNow(resource);
    check(!result.ok);
    check(!result.cancelled);
    check(result.error == "HTTP 404");
    check(!resource.isAvailable());
    check(entriesIn(directory) == 0);

    server.stop();
};

auto tCancel = test("OnlineResource/cancelResolvesCancelledWithNoFile") = []
{
    auto server = StaticFileServer {std::string(1 << 20, 'z')};
    auto directory = scratchDirectory("cancel");

    auto resource = OnlineResource {{"Data", server.url("big.bin")}, directory};
    auto pending = resource.start();
    check(resource.isRunning());
    resource.cancel();

    auto result = pending.waitFor(fetchTimeout);
    check(result.cancelled);
    check(!result.ok);
    check(result.error.empty());
    check(!resource.isRunning());
    check(!exists(directory / "big.bin"));
    check(!exists(directory / "big.bin.part"));
};

auto tSecondFetchRefused =
    test("OnlineResource/aSecondFetchWhileInFlightIsRefused") = []
{
    auto server = StaticFileServer {std::string(1 << 20, 'z')};
    auto directory = scratchDirectory("twice");

    auto resource = OnlineResource {{"Data", server.url("big.bin")}, directory};
    auto first = resource.start();
    auto second = resource.start();

    check(second.isReady());
    auto refused = second.waitFor(fetchTimeout);
    check(!refused.ok);
    check(!refused.cancelled);
    check(!refused.error.empty());

    check(first.waitFor(fetchTimeout).ok);
};

auto tDestroyedMidFetch = test("OnlineResource/destroyingMidFetchNeverResolves") = []
{
    auto gate = StallGate {};
    auto options = ServerOptions {};
    options.threading = ServerThreadingMode::ThreadPool;

    auto server = Server {options};
    check(server.listen(0,
                        [&](const Request&)
                        {
                            gate.wait();
                            return okResponse("late");
                        }));

    auto directory = scratchDirectory("destroyed");
    auto resource = std::make_unique<OnlineResource>(
        OnlineResource::Info {"Data", baseUrl(server.boundPort()) + "/late.txt"},
        directory);

    auto resolved = false;
    resource->start().then([&](OnlineResource::Result) { resolved = true; });
    resource.reset();
    gate.release();

    eacp::Threads::runEventLoopFor(eacp::Time::MS {300});
    check(!resolved);

    server.stop();
};

auto tFetchAsync = test("OnlineResource/fetchAsyncOwnsTheObjectAndCallsBack") = []
{
    auto server = StaticFileServer {std::string(65536, 'a'), "\"v1\""};
    auto directory = scratchDirectory("async");

    auto progressCalls = 0;
    auto lastStage = OnlineResource::Progress::Stage::idle;
    auto finished = std::optional<OnlineResource::Result> {};

    auto options = OnlineResource::Options {};
    options.name = "Data";
    options.url = server.url("data.bin");
    options.directory = directory;
    options.progressInterval = eacp::Time::MS {5};
    options.onProgress = [&](const OnlineResource::Progress& progress)
    {
        ++progressCalls;
        lastStage = progress.stage;
    };
    options.onFinished = [&](const OnlineResource::Result& result)
    { finished = result; };

    auto result = OnlineResource::fetchAsync(options).waitFor(fetchTimeout);

    check(result.ok);
    check(result.downloaded);
    check(finished.has_value());
    check(finished->path == directory / "data.bin");
    check(progressCalls >= 1);
    check(lastStage == OnlineResource::Progress::Stage::done);
    check(readAll(result.path) == server.body);
};

auto tFetchBlocking = test("OnlineResource/fetchBlocksUntilTheResourceIsOnDisk") = []
{
    auto server = StaticFileServer {"blocking", "\"v1\""};
    auto directory = scratchDirectory("blocking");

    auto finishedBeforeReturn = false;

    auto options = OnlineResource::Options {};
    options.name = "Data";
    options.url = server.url("data.txt");
    options.directory = directory;
    options.onFinished = [&](const OnlineResource::Result&)
    { finishedBeforeReturn = true; };

    auto result = OnlineResource::fetch(options);

    check(result.ok);
    check(result.downloaded);
    check(finishedBeforeReturn);
    check(readAll(result.path) == "blocking");

    // Present and validated by the sidecar: no download the second time.
    auto again = OnlineResource::fetch(options);
    check(again.ok);
    check(!again.downloaded);
};

auto tFetchTimesOut = test("OnlineResource/fetchThrowsAfterItsTimeout") = []
{
    auto gate = StallGate {};
    auto serverOptions = ServerOptions {};
    serverOptions.threading = ServerThreadingMode::ThreadPool;

    auto server = Server {serverOptions};
    check(server.listen(0,
                        [&](const Request&)
                        {
                            gate.wait();
                            return okResponse("late");
                        }));

    auto options = OnlineResource::Options {};
    options.name = "Late";
    options.url = baseUrl(server.boundPort()) + "/late.txt";
    options.directory = scratchDirectory("timeout");
    options.timeout = eacp::Time::MS {100};

    auto finished = std::optional<OnlineResource::Result> {};
    options.onFinished = [&](const OnlineResource::Result& result)
    { finished = result; };

    auto message = std::string {};

    try
    {
        OnlineResource::fetch(options);
    }
    catch (const std::runtime_error& error)
    {
        message = error.what();
    }

    check(message.find("Could not fetch Late") != std::string::npos);

    // The transfer itself gave up: the worker has reported back, so the
    // caller can exit without a thread still inside the request.
    check(finished.has_value());
    check(!finished->ok);
    check(!finished->cancelled);
    check(!finished->error.empty());
    check(!exists(options.directory / "late.txt.part"));

    gate.release();
    server.stop();
};

auto tFetchThrowsOnFailure =
    test("OnlineResource/fetchThrowsWhenTheServerSaysNo") = []
{
    auto server = Server();
    check(server.listen(0,
                        [](const Request&)
                        {
                            auto response = Response();
                            response.statusCode = 404;
                            return response;
                        }));

    auto options = OnlineResource::Options {};
    options.url = baseUrl(server.boundPort()) + "/missing.bin";
    options.directory = scratchDirectory("throws");

    auto message = std::string {};

    try
    {
        OnlineResource::fetch(options);
    }
    catch (const std::runtime_error& error)
    {
        message = error.what();
    }

    check(message.find("missing.bin") != std::string::npos);
    check(message.find("HTTP 404") != std::string::npos);

    server.stop();
};

auto tRemove = test("OnlineResource/removeDeletesCopyAndSidecar") = []
{
    auto server = StaticFileServer {"data", "\"v1\""};
    auto directory = scratchDirectory("remove");

    auto resource = OnlineResource {{"Data", server.url("data.txt")}, directory};
    check(fetchNow(resource).ok);
    check(resource.isAvailable());

    check(resource.remove());
    check(!resource.isAvailable());
    check(entriesIn(directory) == 0);

    check(fetchNow(resource).downloaded);
};
